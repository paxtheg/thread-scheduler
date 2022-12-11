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

/*
Functia elimina primul element din coada si il afiseaza
*/
void *pop_front(Queue_t *q)
{
	if (!q->front)
		return NULL;

	List_t elem = q->front;
	void *value = elem->value;
	
	q->front = elem->next;
	free(elem);

	return value;
}

/* 
Functia intoarce primul element din coada sau NULL cand coada este goala
*/
void *queue_front(Queue_t *q)
{
	if (q->front == NULL)
		return NULL;

	return q->front->value;
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

/* 
	Functie care e utilizata pentru creearea unei cozi de 
threaduri care sunt in starea TERMINATED
*/
int check_terminated_thread_priority(void *ads)
{
	return 0;
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

	scheduler->terminated = new_queue(check_terminated_thread_priority);
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
			new_queue(check_terminated_thread_priority);
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
	Functie care introduce in scheduler un nou thread si
ii planifica executia; intoarce id ul threadului
*/
tid_t so_fork(so_handler *func, unsigned int priority)
{
	Thread_t *thread;

	if (priority > SO_MAX_PRIO || func == NULL)
		return INVALID_TID;

	thread = (Thread_t *) malloc(sizeof(Thread_t));
	if (thread == NULL)
		return INVALID_TID;

	thread->priority = priority;
	thread->state = NEW;
	thread->time_remaining = scheduler->time_quantum;
	thread->handler = func;
	sem_init(&thread->is_planned, 0, 0);
	sem_init(&thread->is_running, 0, 0);
	(scheduler->threads_nr)++;

	
	if (pthread_create(&thread->id, NULL, thread_func, thread) != 0) {
		free(thread);
		return INVALID_TID;
	}

	sem_wait(&thread->is_planned);
	/*asteapta pana ce threadul a fost executat*/

	if (scheduler->running != thread)
		so_exec();
	/*executa o instructiune*/

	return thread->id;
}

/*
	Functie care blocheaza threadul pana cand primeste o 
operatie de executat; intoarce 0 daca nu intampina vreo 
eroare sau -1 in caz contrar
*/
int so_wait(unsigned int io)
{

	if (scheduler->io <= io)
		return -1;

	Thread_t *thread = scheduler->running;
	thread->state = WAITING;
	push_back(scheduler->waiting[io], thread);

	scheduler->running = pop_front(scheduler->ready);
	if (scheduler->running != NULL)
		sem_post(&scheduler->running->is_running);
	/*paseaza executia la urmatorul thread in starea READY*/

	sem_wait(&thread->is_running);
	/*asteapta pana cand se afla in starea RUNNING*/

	return 0;
}

/*
	Functia deblocheaza toate threadurile care asteapta IO ul corespunzator
Intoarce numarul de threaduri deblocate daca nu intampina vreo 
eroare sau -1 in caz contrar
*/
int so_signal(unsigned int io)
{
	if (io >= scheduler->io)
		return -1;

	Thread_t *thread = pop_front(scheduler->waiting[io]);
	int cnt = 0;

	while (thread != NULL) {
		thread->state = READY;
		push_back(scheduler->ready, thread);

		cnt++;
		thread = pop_front(scheduler->waiting[io]);
	}

	/*executa o instructiune*/
	so_exec();
	return cnt;
}

/*
	Functia simuleaza executia unei instructiuni, scade quantum 
in cazul unui thread in starea RUNNING si intrerupe 
un thread daca este necesar
*/
void so_exec(void)
{
	if (scheduler->running == NULL)
		return;

	Thread_t *thread = scheduler->running;

	thread->time_remaining--;
	if (thread->time_remaining == 0) {
		thread->state = READY;
		thread->time_remaining = scheduler->time_quantum;
		push_back(scheduler->ready, thread);
		/* se muta threadul in starea READY in coada ready */

		Thread_t *next_thread = pop_front(scheduler->ready);
		/* urmatorul thread in starea READY care nu s-a terminat */

		while (next_thread && next_thread->state == TERMINATED) {
			push_back(scheduler->terminated, next_thread);
			next_thread = pop_front(scheduler->ready);
		}

		scheduler->running = next_thread;
		if (scheduler->running != NULL) {
			/* se seteaza threadul in starea RUNNING */
			scheduler->running->state = RUNNING;
			sem_post(&scheduler->running->is_running);
		}
	} else if (queue_front(scheduler->ready) &&
		thread->priority < 
		((Thread_t *)queue_front(scheduler->ready))->priority) {
		/* swap intre threadul curent in starea RUNNING si threadul 
		cu cea mai mare prioritate*/
		thread->state = READY;
		push_back(scheduler->ready, thread);
		scheduler->running = pop_front(scheduler->ready);
		scheduler->running->state = RUNNING;
		sem_post(&scheduler->running->is_running);
	}

	
	/*daca un thread a fost intrerupt, se asteapta pana cand se 
	intoarce in starea RUNNING*/
	if (thread != scheduler->running)
		sem_wait(&thread->is_running);	
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

/*
	Functia planifica un nou thread si ii seteaza 
starea pe RUNNING sau READY
*/
void plan_new_thread(Thread_t *thread)
{
	if (scheduler->running == NULL) {
		thread->state = RUNNING;
		scheduler->running = thread;
		sem_post(&scheduler->running->is_running);
		return;
	}

	if (thread->priority > scheduler->running->priority) {
		Thread_t *aux = scheduler->running;
		scheduler->running = thread;
		thread->state = RUNNING;
		aux->state = READY;
		push_back(scheduler->ready, aux);
		return;
	}

	thread->state = READY;
	push_back(scheduler->ready, thread);
}

/* Functie care opreste un thread*/
void thread_finished(void)
{
	Thread_t *thread = scheduler->running;

	if (thread->state == TERMINATED) {
		push_back(scheduler->terminated, thread);
		scheduler->running = pop_front(scheduler->ready);

		if (scheduler->running != NULL)
			sem_post(&scheduler->running->is_running);

		if (scheduler->running == NULL && queue_front(scheduler->ready) == NULL)
			sem_post(&scheduler->stop);

	}

}

/* Functie care organizeaza rutina threadurilor */
void *thread_func(void *args)
{
	Thread_t *thread = (Thread_t *) args;

	/* planifica threadul curent si seteaza is_planned */
	plan_new_thread(thread);
	sem_post(&thread->is_planned);

	/* asteapta pana cand threadul se afla in starea RUNNING */
	sem_wait(&thread->is_running);

	thread->handler(thread->priority);
	
	thread->state = TERMINATED;
	thread_finished();

	return NULL;
}

