#include "so_scheduler.h"
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>

/* Declar tipul de functie *TFreeElem care sterge informatii din noduri */
typedef void (*TFreeElem)(void *);

/* 
Declar tipul de functie *TPriorityFunc care verifica prioritatea 
elementelor unei cozi
*/
typedef int (*TPriorityFunc)(void *);


/* Declar structurile de nod si lista pentru creearea cozii */
typedef struct Node {
	void *value; 
	struct Node *next; 
} Node_t, *List_t;

/* Definesc starile posibile are threadurilor */
typedef enum {
	NEW,
	READY,
	RUNNING,
	WAITING,
	TERMINATED
} State;

/*
	Functie care adauga un nod nou in lista; intoarce NULL daca esueaza 
sau nodul adaugat daca reuseste
*/
List_t add_node(void *value)
{
	List_t node = (List_t) malloc(sizeof(Node_t));

	if (node != NULL) {
		node->value = value;
		node->next = NULL;
	}

	return node;
}

/*
	Functie care elibereaza memoria alocata listei, folosindu-se de 
functia free_elem pentru a elibera fiecare element
*/
void free_list(List_t *list_ads, TFreeElem free_elem)
{
	List_t list = *list_ads;

	while (list != NULL) {
		List_t current = list;
		
		list = list->next;
		free_elem(current->value);
		free(current);
	}
}

/*
Definesc structura cozii de prioritate
*/
typedef struct {
	List_t front, back; 
	TPriorityFunc priority; 
} Queue_t;

/*
Functie care initializeaza o noua coada; 
*/
Queue_t *new_queue(TPriorityFunc priority_func)
{
	Queue_t *q = (Queue_t *) malloc(sizeof(Queue_t));

	if (q != NULL) {
		q->front = q->back = NULL;
		q->priority = priority_func;
	}

	return q;
}

/*
	Functie care elibereaza memoria folosita de coada, folosindu-se 
de free_elem pentru eliberarea memoriei elementelor
*/
void free_queue(Queue_t **q, TFreeElem free_elem)
{
	free_list(&((*q)->front), free_elem);
	free(*q);
}

/*
	Functie care introduce un element in coada in functie de 
prioritatea acestuia; intoarce 0 in caz de reustia 
si -1 in caz de esec
*/
int push_back(Queue_t *q, void *value)
{
	List_t elem = add_node(value);
	
	if (elem == NULL)
		return -1;

	if (q->front == NULL) {
		q->front = q->back = elem;
		return 0;
	}
	/* adauga primul element */

	if (q->priority(q->back->value) >= q->priority(value)) {
		q->back->next = elem;
		q->back = elem;
		return 0;
	}
	/*pune elementul in spatele(la coada) cozii*/

	List_t list = q->front, cnt = NULL;

	while ((q->priority(value) <= q->priority(list->value)) && (list != NULL)) {
		cnt = list;
		list = list->next;
	}
	/*
	parcurge lista q->front pana cand prioritatea lui elem este mai mare
	*/

	if (cnt != NULL) {
		elem->next = list;
		cnt->next = elem;
		/*se plaseaza elem intre cnt si list*/
	} else {
		elem->next = q->front;
		q->front = elem;
		/*elem devine primul element din lista*/
		
	}
	return 0;
}

/* Definesc structura pentru un thread dintr-un scheduler care contine:
- prioritatea threadului
- id ul threadului
- functia handler
- starea threadului (NEW, READY, RUNNING, WAITING, TERMINATED)
- timpul ramas al threadului (folosit cand se afla in starea running)
- doua elemente de sincronizare semaforizata care marcheaza daca threadul 
a fost planificat si daca se afla in starea RUNNING
*/
typedef struct {
	int priority; 
	pthread_t id; 
	so_handler *handler; 

	State state; 
	unsigned int time_remaining; 
	sem_t is_planned;
	sem_t is_running;
} Thread_t;

/* Definesc structura schedulerului, care contine:
- numarul de threaduri
- cuanta de timp
- numarul de dispozitive IO
- o coada pentruthreaduri in starea READY
- o coada pentru threaduri in starea TERMINATED
- un vector de cozi in starea WAITING
- threadul curent care se afla in starea RUNNING
- un semafor care marcheaza momentul cand se poate apela functia so_end
*/
typedef struct {
	int threads_nr; 
	unsigned int time_quantum; 
	unsigned int io;
	Queue_t *ready; 
	Queue_t *terminated; 
	Queue_t **waiting; 
	Thread_t *running; 
	sem_t stop; 
} Scheduler_t;

/* Functie care intoarce prioritatea unei cozi in starea READY*/
int thread_priority(void *ads)
{
	return ((Thread_t *) ads)->priority;
}



Scheduler_t *scheduler;

void *thread_func(void *args);
void plan_new_thread(Thread_t *t);
void thread_finished();

/*
	Functie care initializeaza un scheduler si campurile sale;
intoarce 0 pentru success si -1 pentru esec.
*/
int so_init(unsigned int time_quantum, unsigned int io)
{
	if (scheduler != NULL ||
		io > SO_MAX_NUM_EVENTS || time_quantum <= 0)
		return -1;

	scheduler = (Scheduler_t *) malloc(sizeof(Scheduler_t));
	if (scheduler == NULL)
		return -1;

	scheduler->time_quantum = time_quantum;
	scheduler->io = io;
	scheduler->running = NULL;

	scheduler->terminated = new_queue(0);
	if (scheduler->terminated == NULL) {
		free_queue(&scheduler->ready, free);
		free(scheduler);
		return -1;
	}
	/*initializeaza coada de threaduri in starea TERMINATED*/

	scheduler->waiting = (Queue_t **) calloc(io, sizeof(Queue_t *));
	if (scheduler->waiting == NULL) {
		free_queue(&scheduler->ready, free);
		free_queue(&scheduler->terminated, free);
		free(scheduler);
		return -1;
	}

	for (int i = 0; i < io; i++) {
		scheduler->waiting[i] =
			new_queue(0);
		if (scheduler->waiting[i] == NULL)
			return -1;
	}
	/*initializeaza coada de threaduri in starea WAITING*/

	scheduler->ready = new_queue(thread_priority);
	if (scheduler->ready == NULL) {
		free(scheduler);
		return -1;
	}
	/*initializeaza coada de threaduri in starea READY*/
	

	scheduler->threads_nr = 0;
	sem_init(&scheduler->stop, 0, 0);

	return 0;
}


/*
	Functia elimina memoria aferenta threadului
*/
void free_thread(void *ads)
{
	Thread_t *thread = (Thread_t *) ads;

	pthread_join(thread->id, NULL);
	sem_destroy(&thread->is_planned);
	sem_destroy(&thread->is_running);
	free(thread);
}

/*
	Functia asteapta ca toate threadurile sa se termine si 
elibereaza resursele scheduler ului
*/
void so_end(void)
{
	if (scheduler != NULL) {
		/* asteapta pana cand se termina threadurile */
		if (scheduler->threads_nr != 0)
			sem_wait(&scheduler->stop);

		free_queue(&scheduler->terminated, free_thread);
		free_queue(&scheduler->ready, free_thread);

		if (scheduler->running)
			free_thread(&scheduler->running);

		for (int i = 0; i < scheduler->io; i++)
			free_queue(&scheduler->waiting[i], free);

		free(scheduler->waiting);
		free(scheduler);
		sem_destroy(&scheduler->stop);
	}

	scheduler = NULL;
}

tid_t so_fork(so_handler *func, unsigned int priority){
	return INVALID_TID;
}

void so_exec(void){
}

int so_wait(unsigned int io){
	return 0;
}

int so_signal(unsigned int io){
	return 0;
}
