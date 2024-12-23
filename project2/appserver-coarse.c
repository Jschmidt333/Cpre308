#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h> 
#include <unistd.h> 
#include <pthread.h>
#include <sys/time.h>

#include "Bank.h"

struct trans { // structure for a transaction pair
    int acc_id; // account ID
    int amount; // amount to be added, could be positive or negative
};

// Linked list node
struct request
{
    struct request * next;              // pointer to the next request in the list
    int request_id;                     // request ID assigned by the main thread
    int check_acc_id;                   // account ID for a CHECK request
    struct trans * transactions;        // array of transaction data
    int num_trans;                      // number of accounts in this transaction
    struct timeval starttime, endtime;  // starttime and endtime for TIME 
};

// Linked list of jobs
struct queue
{
    struct request *head, *tail;
    int num_jobs;
};

void initialize_mutexlocks();
void tokenize();
void* thread_worker();
int check_transactions_valid(struct request* transaction);
void join_all_threads(pthread_t* worker_threads_array, int num_threads);
void initialize_workers(pthread_t* worker_threads_array, int num_threads);
void print_q();

// Tells worker threads to stop
volatile int stop_flag = 0;
pthread_mutex_t q_lock;
pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
struct queue q;
pthread_mutex_t mutex_lock; // Singular for entire bank
FILE* output_file;
int q_count = 0;
pthread_mutex_t account_lock;

int main(int argc, char* argv[])
{

    if(argc != 4)
    {
        printf("Arguments = %d, should be 4\n", argc);
        return 0;
    }

    // Initialization
    int num_threads = atoi(argv[1]);
    int num_accounts = atoi(argv[2]);
    char* file_path = argv[3];
    if(num_threads == 0 || num_accounts == 0)
    {
        printf("# of worker threads or # of accounts invalid input");
        return 0;
    }

    output_file = fopen(file_path, "w+");
    if(output_file == NULL)
    {
        printf("No such file: %s", file_path);
        return 0;
    }

    initialize_accounts(num_accounts);

    char buffer[160];
    char tokens[30][30];
    int tokens_count = 0;

    if(pthread_mutex_init(&mutex_lock, NULL) != 0) printf("failed to create mutex\n");
    if(pthread_mutex_init(&account_lock, NULL) != 0) printf("FAILED TO initialize q mutex lock\n");

    pthread_t* worker_threads_array = (pthread_t*)malloc(sizeof(pthread_t) * num_threads);
    if (!worker_threads_array) {
        printf("Failed to allocate memory for worker threads array\n");
        return 1; // Exit or handle error
    }
    initialize_workers(worker_threads_array, num_threads);

    if(pthread_mutex_init(&q_lock, NULL) != 0) printf("FAILED TO initialize q mutex lock\n");

    // Initalize the queue with all null values
    q.head = NULL, q.tail = NULL, q.num_jobs = 0;

    // To keep track of the current id requested
    int request_id = 1;

    // Main Logic
    while(1)
    {
        fgets(buffer, sizeof(buffer), stdin);
        tokenize(buffer, tokens, &tokens_count);
        if(strcmp(tokens[0], "CHECK") == 0)
        {
            // Balance check
            struct timeval time;
            gettimeofday(&time, NULL);

            int given_id = atoi(tokens[1]);

            // // Make a task and add it to the end of the queue for a worker thread to pick up
            struct request *cur_request = (struct request *)malloc(sizeof(struct request));
            *cur_request = (struct request){.request_id = request_id, .check_acc_id = given_id, .starttime = time};
            pthread_mutex_lock(&q_lock);
            q_count++;
            struct request* last_request = q.tail;
            if(last_request == NULL)
            {
                q.head = cur_request;
                q.tail = cur_request;
            }
            else
            {
                last_request->next = cur_request;
                q.tail = cur_request;
            }
            //printf("head: %d tail %d\n", q.head->request_id, q.tail->request_id);
            printf("ID %d\n", request_id);
            request_id++;
            pthread_cond_broadcast(&q_cond);
            pthread_mutex_unlock(&q_lock);
        }
        else if(strcmp(tokens[0], "TRANS") == 0)
        {
            // Request transaction
            struct timeval time;
            gettimeofday(&time, NULL);

            // Go through and get all the transactions from the given string
            int number_transactions = (tokens_count - 1) / 2;
             struct trans *transactions = malloc(number_transactions * sizeof(struct trans)); // Minus 1 for the first prompt in the string
            for(int i = 1; i < tokens_count; i+=2)
            {
                transactions[i/2].acc_id = atoi(tokens[i]);
                transactions[i/2].amount = atoi(tokens[i+1]);
            }

            // Add it to the queue
            struct request *cur_request = (struct request *)malloc(sizeof(struct request));
            *cur_request = (struct request){.request_id = request_id, .transactions = transactions, .num_trans = number_transactions, .starttime = time};
            request_id++;
            
            pthread_mutex_lock(&q_lock);
            if (q.tail == NULL) {
                q.head = cur_request;
            } else {
                q.tail->next = cur_request;
            }
            q_count++;
            fflush(stdout);
            q.tail = cur_request;
            pthread_cond_broadcast(&q_cond);
            pthread_mutex_unlock(&q_lock);
            
        }
        else if (strcmp(tokens[0], "END") == 0)
        {
            while(q_count > 0) usleep(100);
            stop_flag = 1;
            pthread_cond_broadcast(&q_cond);  // Wake all threads to terminate
            join_all_threads(worker_threads_array, num_threads);
            // Cleanup
            fclose(output_file);
            free(worker_threads_array);
            free_accounts();
            return 0;
        }
        else
        {
            printf("INVALID INPUT\n");
        }
    }

}

void* thread_worker(void* arg) {
    
    while (!stop_flag) {
        
        pthread_mutex_lock(&q_lock);

        // Wait until there is a request or stop flag
        while (q.head == NULL && !stop_flag) pthread_cond_wait(&q_cond, &q_lock);
        //while (q.head == NULL && !stop_flag) usleep(1);

        if (stop_flag && q.head == NULL) {
            pthread_mutex_unlock(&q_lock);
            break;
        }

        q_count--;
        fflush(stdout);

        struct request* cur_trans = q.head;
        q.head = cur_trans->next;
        if (q.head == NULL) {
            q.tail = NULL;
        }
        pthread_mutex_lock(&account_lock);
        pthread_mutex_unlock(&q_lock);

        struct timeval endtime;
        gettimeofday(&endtime, NULL);

        if (cur_trans->check_acc_id != 0) {
            // Process CHECK
            int account_id = cur_trans->check_acc_id;
            pthread_mutex_lock(&mutex_lock);  // Lock the entire bank for a CHECK
            pthread_mutex_unlock(&account_lock);
            int amt = read_account(account_id);
            pthread_mutex_unlock(&mutex_lock);
            fprintf(output_file, "%d BAL %d TIME %ld.%06ld %ld.%06ld\n", cur_trans->request_id, amt,
                    cur_trans->starttime.tv_sec, cur_trans->starttime.tv_usec, endtime.tv_sec, endtime.tv_usec);
        } else {
            // Process TRANS
            int acct_num;
            pthread_mutex_lock(&mutex_lock);  // Lock the bank for the entire transaction processing
            pthread_mutex_unlock(&account_lock);
            acct_num = check_transactions_valid(cur_trans);
            pthread_mutex_unlock(&mutex_lock);

            if (acct_num == 0) {
                fprintf(output_file, "%d OK TIME %ld.%06ld %ld.%06ld\n", cur_trans->request_id,
                        cur_trans->starttime.tv_sec, cur_trans->starttime.tv_usec, endtime.tv_sec, endtime.tv_usec);
            } else {
                fprintf(output_file, "%d ISF %d TIME %ld.%06ld %ld.%06ld\n", cur_trans->request_id, acct_num,
                        cur_trans->starttime.tv_sec, cur_trans->starttime.tv_usec, endtime.tv_sec, endtime.tv_usec);
            }
        }

        free(cur_trans->transactions);
        free(cur_trans);
    }

    usleep(100);

    pthread_exit(NULL);
}

int compare_transactions(const void* a, const void* b)
{
    return ((struct trans*)a)->acc_id - ((struct trans*)b)->acc_id;
}

int check_transactions_valid(struct request* transaction) {

    qsort(transaction->transactions, transaction->num_trans, sizeof(struct trans), compare_transactions);

    int tentative_balances[transaction->num_trans];
    int i;

    for (i = 0; i < transaction->num_trans; i++) {
        int acc_id = transaction->transactions[i].acc_id;
        
        // Calculate tentative balance
        int current_balance = read_account(acc_id);
        tentative_balances[i] = current_balance + transaction->transactions[i].amount;

        // If tentative balance is insufficient, release the lock and return
        if (tentative_balances[i] < 0) {
            return transaction->transactions[i].acc_id; // Return the ID of the insufficient account
        }
    }

    // If all transactions are valid, update the accounts
    for (i = 0; i < transaction->num_trans; i++) {
        int acc_id = transaction->transactions[i].acc_id;
        write_account(acc_id, tentative_balances[i]);
    }

    return 0;

}


void tokenize(char str[], char tokens[][30], int* count)
{
    
    char* token = strtok(str, " ");
    int i = 0;
    char* pos;
    while (token) {
        pos = strchr(token, '\n');
        if(pos != NULL) 
        {
            token[pos - token] = '\0';
        }
        strcpy(tokens[i], token);
        token = strtok(NULL, " ");
        i++;
    }

    *count = i;

}

void* chop(void* arg)
{
    pthread_exit(NULL);
}

void initialize_workers(pthread_t* worker_threads_array, int num_threads)
{
    for(int i = 0; i < num_threads; i++)
    {
        if(pthread_create(&worker_threads_array[i], NULL, thread_worker, NULL) != 0) printf("failed to create mutex\n");
    }
}

void join_all_threads(pthread_t* worker_threads_array, int num_threads)
{
    for(int i = 0; i < num_threads; i++)
    {
        pthread_join(worker_threads_array[i], NULL);
    }
}

void print_q()
{
    struct request* current = q.head;
    while(current != NULL)
    {
        current = current->next;
    }
}
