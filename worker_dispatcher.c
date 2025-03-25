#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>

#define MAX_LINE 1024
#define COUNTERS_AMOUNT 100
#define MAX_THREADS 4096

// for counters 
pthread_mutex_t *counter_file_mutex; //pointer to counter files mutexes arrays
pthread_cond_t *counter_file_cond;  //pointer to counter files conditions array
FILE *counter_files[COUNTERS_AMOUNT]; //pointer to counter files array

// for jobs
pthread_mutex_t jobs_mutex = PTHREAD_MUTEX_INITIALIZER; //dispatcher wait
pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;   //dispatcher wait
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

int jobs_count=0;// total number of jobs
int jobs_in_queue = 0; //jobs waiting queue
static int shutdown_flag = 0;  //for shutting down worker threads

// for statistics
static long long sum_exec_time = 0;
static long long min_exec_time = -1;
static long long max_exec_time = -1;
static int total_jobs = 0;


// Structures
struct thread_data {
    int id;
};

typedef struct job_t { //job queue node
    char *cmd_line;
    struct job_t *next;
    long long read_time;
} job_t;

static job_t *queue_first = NULL;
static job_t *queue_last = NULL;
static struct timeval start_time;

int log_enabled = 0;


void *worker_thread_func(void *arg);
void initialize_counter_files(int counter_num);
long long get_time_in_milliseconds();
void update_count_file(int count_idx, int delta);
void handle_dispatcher_command(const char *line);
void handle_worker_command(const char *line);
void exec_single_worker_cmd(char *cmd);
void add_job_to_queue(char *line, long long read_time);
job_t *remove_job_from_queue();
void handle_cmd_line(char line[],long long read_time);
void update_log_files(int thread_num, const char *job_line, const char *status);
void read_cmd_line(FILE *cmdf, int num_threads);
char *remove_spaces(const char *str);
void finish_program(int num_threads,  pthread_t tid []);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Functions Implementation
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void handle_cmd_line(char line[],long long read_time) 
{ 
    char *newline = remove_spaces(line);
    if(strncmp(newline, "dispatcher", 10) == 0)  
    {
        handle_dispatcher_command(newline);
    } 
    else if(strncmp(newline, "worker", 6) == 0) 
    {
        add_job_to_queue(newline,read_time);
    }
    else 
    {
        printf("Unknown command: %s\n", newline);
    }
    free(newline);
}


void exec_single_worker_cmd(char *cmd)
{
    int arg;
    if (strncmp(cmd,"msleep",6) == 0)
    {
        arg = atoi(cmd+6);
        usleep(arg*1000);
    }
    else if (strncmp(cmd,"increment",9) == 0)
    {
        arg = atoi(cmd+9);
        update_count_file(arg, 1);
    }
    else if (strncmp(cmd,"decrement",9) == 0)
    {
        arg = atoi(cmd+9);
        update_count_file(arg, -1);
    } 
}

void handle_dispatcher_command(const char *line) 
{
    if(strncmp(line, "dispatcher_msleep", 17) == 0) 
    {
        int x = atoi(line+18);
        usleep(x*1000);
    } 
    else if(strncmp(line, "dispatcher_wait", 15) == 0) 
    {
        // wait for when jobs_pending == 0
        pthread_mutex_lock(&jobs_mutex);
        while(jobs_in_queue > 0) 
        {
            pthread_cond_wait(&jobs_cond, &jobs_mutex);
        }
        pthread_mutex_unlock(&jobs_mutex);
    } 
    else 
    {
        printf("Unknown command: %s\n", line);
    }
}


void *worker_thread_func(void *arg) 
{
    int thread_id = ((struct thread_data*)arg)->id;

    while (true) 
    {
        pthread_mutex_lock(&jobs_mutex);
        while (!queue_first && !shutdown_flag) 
        {
            pthread_cond_wait(&jobs_cond, &jobs_mutex);
        }
        if (!queue_first && shutdown_flag) 
        {
            pthread_mutex_unlock(&jobs_mutex);
            break;
        }
        job_t *job = remove_job_from_queue();
        pthread_mutex_unlock(&jobs_mutex);
        if (!job) 
        {
            continue;
        }
        
        if(log_enabled)
        {
            update_log_files(thread_id, job->cmd_line, "START");
        }

        handle_worker_command(job->cmd_line);
        long long end_exec = get_time_in_milliseconds();

        if(log_enabled)
        {
            update_log_files(thread_id, job->cmd_line, "END");
        }
        
        //update statistics
        long long exec_time = end_exec - job->read_time;
        pthread_mutex_lock(&stats_mutex);
        sum_exec_time += exec_time;
        if (min_exec_time == -1 || exec_time < min_exec_time) min_exec_time = exec_time;
        if (exec_time > max_exec_time) max_exec_time = exec_time;
        total_jobs++;
        
        pthread_mutex_unlock(&stats_mutex);
        free(job);
    }

    return NULL;
}


void handle_worker_command(const char *line)
{
    
    char *start = strstr(line, "worker");
    if(!start) return;
    start += 6; //skip "worker"
    while(*start == ' ' || *start == '\t') 
    {
        start++;
    }

    char *commands_str = strdup(start);
    char *saveptr;
    int count_cmds = 0;
    char *cmd_tokens[512];
    char *token = strtok_r(commands_str, ";", &saveptr);
    while(token && count_cmds < 512) 
    {
        while(*token == ' ' || *token == '\t') 
        {
            token++;
        }
        char *end = token + strlen(token)-1;
        while(end > token && (*end==' ' || *end=='\t')) 
        {
            *end = '\0';
            end--;
        }
        cmd_tokens[count_cmds++] = token;
        token = strtok_r(NULL, ";", &saveptr);
    }
    
    //execution of the job
    //find repeat, its index and times to repeat
    bool has_repeat = false;
    int repeat_times = 0;
    int repeat_index = count_cmds;
    for(int i = 0; i<count_cmds; i++)
    {
        if(cmd_tokens[i] != NULL)
        {
            if (strncmp(cmd_tokens[i], "repeat", 6) == 0)
            {
                has_repeat=true;
                repeat_times = atoi(cmd_tokens[i]+6);
                repeat_index=i;
                break;
            }
        }
    }

    //if theres a repeat execute commands until repeat index, if not execute all commands. then execute the command after the repeat repeat_times times serially if theres a repeat
    for(int i = 0; i<repeat_index; i++)
    {
        exec_single_worker_cmd(cmd_tokens[i]);
    }
    if(has_repeat)
    {
        for(int i = 0; i<repeat_times; i++)
        {
            for(int j = repeat_index+1; j<count_cmds; j++)
            {
                exec_single_worker_cmd(cmd_tokens[j]);
            }
        }
    }
    free(commands_str);
}

void add_job_to_queue(char *line, long long read_time)
{
    struct job_t *new = (job_t*)malloc(sizeof(struct job_t));
    new->read_time = read_time;
    new->cmd_line = strdup(line);
    new->next= NULL;
    pthread_mutex_lock(&jobs_mutex);
    if(queue_last == NULL)
    {
        queue_first = new;
        queue_last = new;
    }else
    {
        queue_last->next = new;
        queue_last = new;
    }
    jobs_in_queue++;
    jobs_count++;
    pthread_cond_broadcast(&jobs_cond);
    pthread_mutex_unlock(&jobs_mutex);
}

job_t* remove_job_from_queue()
{
    if(!queue_first)
    {
        return(NULL);
    }
    job_t *job = queue_first;
    queue_first = queue_first->next;
    if(queue_first == NULL)
    {
        queue_last = NULL;
    }
    jobs_in_queue--;
    //notify when queue is empty
    if(jobs_in_queue == 0)
    {
        pthread_cond_broadcast(&jobs_cond);
    }
    return(job);
}

void update_count_file(int count, int increase)
{
    pthread_mutex_lock(&counter_file_mutex[count]);//lock file mutex
    char filename[16];
    snprintf(filename, sizeof(filename), "count%02d.txt",count);
    FILE *file = fopen(filename,"r+");
    if(file ==NULL)
    {
        printf("Error: file %s was not opened properly\n",filename);
        pthread_mutex_unlock(&counter_file_mutex[count]);
        exit(1);
    }
    //get the value in the file
    int value;
    fscanf(file,"%d\n",&value);
    value = value + increase;
    rewind(file);
    fprintf(file,"%d\n",value);
    fclose(file);
    pthread_mutex_unlock(&counter_file_mutex[count]);//release file mutex
    pthread_cond_signal(&counter_file_cond[count]);
}


void initialize_counter_files(int counter_num) 
{
    counter_file_mutex = malloc(COUNTERS_AMOUNT * sizeof(pthread_mutex_t));
    counter_file_cond = malloc(COUNTERS_AMOUNT * sizeof(pthread_cond_t));
    if (!counter_file_mutex || !counter_file_cond) 
    {
        fprintf(stderr, "Memory allocation failed\n");
        exit(EXIT_FAILURE);
    }

    char file_name[16]; 

    for (int i = 0; i < counter_num; i++) 
    {
        snprintf(file_name, sizeof(file_name), "count%02d.txt", i);
        counter_files[i] = fopen(file_name, "w");
        if (!counter_files[i]) 
        {
            fprintf(stderr, "File %s not created\n", file_name);
            exit(EXIT_FAILURE);
        }
        fprintf(counter_files[i], "0\n");
        fclose(counter_files[i]);

        if (pthread_mutex_init(&counter_file_mutex[i], NULL) != 0) 
        {
            fprintf(stderr, "Failed to initialize mutex %d\n", i);
            exit(EXIT_FAILURE);
        }
        if (pthread_cond_init(&counter_file_cond[i], NULL) != 0) 
        {
            fprintf(stderr, "Failed to initialize condition %d\n", i);
            exit(EXIT_FAILURE);
        }
    }
}

long long get_time_in_milliseconds() 
{
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    long long elapsed_sec = current_time.tv_sec - start_time.tv_sec;
    long long elapsed_usec = current_time.tv_usec - start_time.tv_usec;
    return elapsed_sec * 1000 + elapsed_usec / 1000;
}

void update_log_files(int thread_num, const char *job_line, const char *status) 
{
    char filename[20];
    snprintf(filename, sizeof(filename), "thread%02d.txt", thread_num);
    FILE *log_file = fopen(filename, "a");
    if (!log_file) 
    {
        perror("Error opening log file");
        return;
    }
    long long timestamp = get_time_in_milliseconds();
    fprintf(log_file, "TIME %lld: %s job %s\n", timestamp, status, job_line);
    fclose(log_file);
}

char *remove_spaces(const char *str) 
{
    size_t len = strlen(str);
    char *result = malloc(len + 1);
    if (!result) 
    {
        perror("Memory allocation failed");
        exit(EXIT_FAILURE);
    }
    size_t j = 0;
    for (size_t i = 0; i < len; i++) 
    {
        if (str[i] != ' ') 
        {
            result[j++] = str[i];
        }
    }
    result[j] = '\0';
    return result;
}

void read_cmd_line(FILE* cmdf,int num_threads)
{
    char cmd_line[MAX_LINE];
    FILE *dispatcher_log_file = NULL;
    if(log_enabled) //if log is enabled write to log file
    {
        dispatcher_log_file = fopen("dispatcher.txt", "a");
        if (dispatcher_log_file == NULL) {
            perror("Error opening dispatcher log file");
            return;
        }
    }

    while (fgets(cmd_line, sizeof(cmd_line), cmdf) != NULL)   //check if line is empty!
    {
        char *new_line = strchr(cmd_line, '\n');
        if (new_line) *new_line = '\0';
        long long read_time = get_time_in_milliseconds();
        if(log_enabled) //if log is enabled write to log file
        {
            fprintf(dispatcher_log_file, "TIME %lld: read cmd line: %s\n", read_time, cmd_line);
        }
        //check command
        handle_cmd_line(cmd_line,read_time);
    }
    if (log_enabled && dispatcher_log_file != NULL) 
    {
        fclose(dispatcher_log_file);  
    }
}
void finish_program(int num_threads,  pthread_t tid [])
{
    //dispatcher.wait to ensure all done
    handle_dispatcher_command("dispatcher_wait");

    //no new jobs -> set shutdown_flag
    pthread_mutex_lock(&jobs_mutex);
    shutdown_flag = 1;
    pthread_mutex_unlock(&jobs_mutex);
    pthread_cond_broadcast(&jobs_cond);
 
    //join all threads
    for(int i=0; i<num_threads; i++) 
    {
        pthread_join(tid[i], NULL);
    }
    
    
    ////////////////////////////
    // statistics
    ////////////////////////////
    
    //total running time
    long long running_time = get_time_in_milliseconds();
    //stats
    FILE *statsf = fopen("stats.txt", "w");
    if (!statsf) 
    {
        fprintf(stderr, "[WARN] Could not open stats.txt for writing.\n");
        return;
    }
    fprintf(statsf, "total running time: %lld milliseconds\n", running_time);
    fprintf(statsf, "sum of jobs turnaround time: %lld milliseconds\n", sum_exec_time);
    if(jobs_count > 0) 
    {
        fprintf(statsf, "min job turnaround time: %lld milliseconds\n", min_exec_time);
        fprintf(statsf, "average job turnaround time: %f milliseconds\n", (double)sum_exec_time /jobs_count);
        fprintf(statsf, "max job turnaround time: %lld milliseconds\n", max_exec_time);
    } 
    else
    {
        fprintf(statsf, "min job turnaround time: 0 milliseconds\n");
        fprintf(statsf, "average job turnaround time: 0.0 milliseconds\n");
        fprintf(statsf, "max job turnaround time: 0 milliseconds\n");
    }
    fclose(statsf);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Main
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[]) 

{
    gettimeofday(&start_time, NULL); // start clock

    if (argc != 5)
    {
        printf("invalid arguments");
        return(1);
    }
    char* cmdfile_name = argv[1];
    int num_threads = atoi(argv[2]);
    int num_counters = atoi(argv[3]);
    log_enabled = atoi(argv[4]);


    FILE *cmdf = fopen(cmdfile_name, "r");
    if (!cmdf) 
    {
        fprintf(stderr, "Error: Could not open command file: %s\n", cmdfile_name);
        return 1;
    }
    
    initialize_counter_files(num_counters);
    //create num_threads new worker threads
    pthread_t tid [num_threads];
    //pthread_attr_t attr;
    struct thread_data t_data[num_threads];
    for (int i = 0; i<num_threads; i++)
    {
        t_data[i].id = i;
        if(pthread_create(&tid[i],NULL, worker_thread_func ,&t_data[i])!=0)
        {
            fprintf(stderr, "Error: Could not create thread %d\n", i);
            fclose(cmdf);
            return 1;
        }
    }
 
    read_cmd_line(cmdf,num_threads);//read a line from the command file
    fclose(cmdf);
    finish_program(num_threads, tid);
    return 0;
}

