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
typedef struct Node
{
    void *value;
    struct Node *next;
} Node_t, *List_t;

/* Definesc starile posibile are threadurilor */
typedef enum
{
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
    List_t node = (List_t)malloc(sizeof(Node_t));

    if (node != NULL)
    {
        node->value = value;
        node->next = NULL;
    }

    return node;
}

/*
Definesc structura cozii de prioritate
*/
typedef struct
{
    List_t front, back;
    TPriorityFunc priority;
} Queue_t;

/*
Functie care initializeaza o noua coada;
*/
Queue_t *new_queue(TPriorityFunc priority_func)
{
    Queue_t *q = (Queue_t *)malloc(sizeof(Queue_t));

    if (q != NULL)
    {
        q->front = NULL;
        q->back = NULL;
        q->priority = priority_func;
    }

    return q;
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
typedef struct
{
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
typedef struct
{
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
    return ((Thread_t *)ads)->priority;
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
    if (io > SO_MAX_NUM_EVENTS || time_quantum <= 0 || scheduler != NULL)
        return -1;

    scheduler = (Scheduler_t *)malloc(sizeof(Scheduler_t));
    if (scheduler == NULL)
        return -1;

    scheduler->time_quantum = time_quantum;
    scheduler->io = io;
    scheduler->running = NULL;

    scheduler->terminated = new_queue(0);
    if (scheduler->terminated == NULL)
    {
        while (scheduler->ready->front != NULL)
        {
            List_t current = scheduler->ready->front;
            scheduler->ready->front = scheduler->ready->front->next;
            free(current->value);
            free(current);
        }
        // free_list(&scheduler->ready->front, free);
        free(scheduler->ready);
        free(scheduler);
        return -1;
    }
    /*initializeaza coada de threaduri in starea TERMINATED*/

    scheduler->waiting = (Queue_t **)calloc(io, sizeof(Queue_t *));
    if (scheduler->waiting == NULL)
    {

        while (scheduler->ready->front != NULL)
        {
            List_t current = scheduler->ready->front;
            scheduler->ready->front = scheduler->ready->front->next;
            free(current->value);
            free(current);
        }
        free(scheduler->ready);

        while (scheduler->terminated->front != NULL)
        {
            List_t current = scheduler->terminated->front;
            scheduler->terminated->front = scheduler->terminated->front->next;
            free(current->value);
            free(current);
        }
        free(scheduler->terminated);

        free(scheduler);
        return -1;
    }

    for (int i = 0; i < io; i++)
    {
        scheduler->waiting[i] =
            new_queue(0);
        if (scheduler->waiting[i] == NULL)
            return -1;
    }
    /*initializeaza coada de threaduri in starea WAITING*/

    scheduler->ready = new_queue(thread_priority);
    if (scheduler->ready == NULL)
    {
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
    Thread_t *thread = (Thread_t *)ads;

    pthread_join(thread->id, NULL);
    sem_destroy(&thread->is_running);
    sem_destroy(&thread->is_planned);

    free(thread);
}

/*
    Functia asteapta ca toate threadurile sa se termine si
elibereaza resursele scheduler ului
*/
void so_end(void)
{
    if (scheduler != NULL)
    {
        /* asteapta pana cand se termina threadurile */
        if (scheduler->threads_nr != 0)
            sem_wait(&scheduler->stop);

        while (scheduler->terminated->front != NULL)
        {
            List_t current = scheduler->terminated->front;
            scheduler->terminated->front = scheduler->terminated->front->next;
            free_thread(current->value);
            free(current);
        }
        free(scheduler->terminated);

        while (scheduler->ready->front != NULL)
        {
            List_t current = scheduler->ready->front;
            scheduler->ready->front = scheduler->ready->front->next;
            free_thread(current->value);
            free(current);
        }
        free(scheduler->ready);

        if (scheduler->running)
        {
            pthread_join(scheduler->running->id, NULL);
            sem_destroy(&scheduler->running->is_running);
            sem_destroy(&scheduler->running->is_planned);
            free(scheduler->running);
        }

        for (int i = 0; i < scheduler->io; i++)
        {
            while (scheduler->waiting[i]->front != NULL)
            {
                List_t current = scheduler->waiting[i]->front;
                scheduler->waiting[i]->front = scheduler->waiting[i]->front->next;
                free(current->value);
                free(current);
            }
            free(scheduler->waiting[i]);
        }

        free(scheduler->waiting);
        free(scheduler);
        sem_destroy(&scheduler->stop);
    }

    scheduler = NULL;
}

/*
 Functia incepe un nou thread si ii planifica executia; 
 intoarce id ul threadului
 */
tid_t so_fork(so_handler *func, unsigned int priority)
{
    Thread_t *thread;

    if (priority > SO_MAX_PRIO || !func)
        return INVALID_TID;

    thread = (Thread_t *)malloc(sizeof(Thread_t));
    if (!thread)
        return INVALID_TID;

    thread->priority = priority;
    thread->state = NEW;
    thread->time_remaining = scheduler->time_quantum;
    thread->handler = func;
    sem_init(&thread->is_planned, 0, 0);
    sem_init(&thread->is_running, 0, 0);
    (scheduler->threads_nr)++;

    if (pthread_create(&thread->id, NULL, thread_func, thread) != 0)
    {
        free(thread);
        return INVALID_TID;
    }

    sem_wait(&thread->is_planned);

    if (scheduler->running != thread)
        so_exec();

    return thread->id;
}

void so_exec(void)
{
}

int so_wait(unsigned int io)
{
    return 0;
}

int so_signal(unsigned int io)
{
    return 0;
}
