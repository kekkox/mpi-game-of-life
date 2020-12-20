#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include<getopt.h>
#include <mpi.h>

#define MASTER_NODE 0
#define MIN_SIZE 3
#define PREV_ROW_TAG 8
#define NEXT_ROW_TAG 9

/** Game functions **/

// Perform the algorithm of life on a specified cell
void decideFate(char *origin_buff, char *result_buff, int index, int live_count);
// Let die a cell
void die(char *cell);
// Let born a cell
void born(char *cell);
// Check if a cell is alive
int isAlive(char cell);
// Wait for prev_request and then perform the logic
void waitAndExecutePrev(MPI_Request* to_wait, char* origin_buff, char* prev_row, char* result_buff, int column_size);
// Perform the logic on the previous row
void executePrev(char* origin_buff, char* prev_row, char* result_buff, int column_size);
// Wait for next_request and then perform the logic
void waitAndExecuteNext(MPI_Request* to_wait, char* origin_buff, char* next_row, char* result_buff, int my_row_size, int column_size);
// Perform the logic on the next row
void executeNext(char* origin_buff, char* next_row, char* result_buff, int my_row_size, int column_size);

/** Util functions **/

// Clear all the screen
void clearScreen();
// Parse the command line arguments
int parseArgs(int argc, char** argv, int* row, int* col, int* iterations, int* verbose);
// Print on stdout how to use this program
void usage();

int main(int argc, char **argv) {

    int rank;               // Rank of the process
    int world_size;         // Number of processes
    double start_time;      // Indicate the starting time
    double end_time;        // Indicate the ending time
    int iteration_count;    // Number of steps to perform
    int column_size;        // Column size
    int row_size;           // Row size
    int my_prev;            // The previous process
    int my_next;            // The next process
    int is_verbose = 0;     // If the program is in verbose mode, the program prints on stdout with small pause
                            // Warning! This impact performance! Please, do it only in testing enviroment

    int *send_count;        // send_count[i] = N° elements to send at process with rank i
    int *displacement;
    char *receive_buff;     // Buffer used for receive the scatter from MASTER_NODE
    char *prev_row;         // Buffer used to store the previous row
    char *next_row;         // Buffer used to store the next row
    char *result_buff;      // Buffer used to store the game computation on a node
    char *game_matrix;      // The game matrix

    MPI_Request send_request = MPI_REQUEST_NULL; //Request used for send data
    MPI_Request prev_request = MPI_REQUEST_NULL; //Request used for receive data from the previous process
    MPI_Request next_request = MPI_REQUEST_NULL; //Request used for receive data from the next process
    MPI_Status status;
    MPI_Datatype game_row;                       //Custom datatype that rappresent a row of the matrix's game

    // Initialize the MPI environment
    MPI_Init(NULL, NULL);
    // Get the number of processes
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    // Get the rank of the process
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    // Parsing arguments
    if(!parseArgs(argc, argv, &row_size, &column_size, &iteration_count, &is_verbose)) {
        if(rank == MASTER_NODE)
            usage();

        MPI_Finalize();
        
        return 0;
    }

    MPI_Type_contiguous(column_size, MPI_CHAR, &game_row);
    MPI_Type_commit(&game_row);

    send_count = calloc(world_size, sizeof(int));
    displacement = calloc(world_size, sizeof(int));
    int base_offset = (int)row_size / world_size;
    int rest = row_size % world_size;
    int displacement_count = 0;

    // Calculating dislacemennt and send_count 
    for (int i = 0; i < world_size; i++) {
        displacement[i] = displacement_count;

        if (rest > 0) {
            send_count[i] = base_offset + 1;
            rest--;
        }
        else
            send_count[i] = base_offset;

        displacement_count += send_count[i];
    }

    // Alloc Matrix
    if (rank == MASTER_NODE) {
        // Getting start time
        start_time = MPI_Wtime();

        // The game matrix is an array treated like a matrix
        game_matrix = calloc(row_size * column_size, sizeof(char));
    }

    // Init Matrix
    result_buff = calloc(send_count[rank] * column_size, sizeof(char));
    srand(time(NULL) + rank);
    for(int i = 0; i < send_count[rank] * column_size; i++)
        if (rand() % 2 == 0)
            born(&result_buff[i]);
        else
            die(&result_buff[i]);

    MPI_Gatherv(result_buff, send_count[rank], game_row, game_matrix, send_count, displacement, game_row, MASTER_NODE, MPI_COMM_WORLD);

    // Calculating my_prev and my_next
    my_prev = rank == 0 ? world_size - 1 : rank - 1;
    my_next = (rank + 1) == world_size ? 0 : rank + 1;

    receive_buff = calloc(send_count[rank] * column_size, sizeof(char));
    result_buff = calloc(send_count[rank] * column_size, sizeof(char));
    prev_row = calloc(column_size, sizeof(char));
    next_row = calloc(column_size, sizeof(char));

    for(int iter = 0; iter < iteration_count; iter++) {

        MPI_Scatterv(game_matrix, send_count, displacement, game_row, receive_buff, send_count[rank], game_row, MASTER_NODE, MPI_COMM_WORLD);
        
        int my_row_size = send_count[rank];

        // Sending my first row to my prev (It will be its next_row)
        MPI_Isend(receive_buff, 1, game_row, my_prev, NEXT_ROW_TAG, MPI_COMM_WORLD, &send_request);
        MPI_Request_free(&send_request);

        // Receiving my prev_row from my_prev
        MPI_Irecv(prev_row, 1, game_row, my_prev, PREV_ROW_TAG, MPI_COMM_WORLD, &prev_request);

        // Sending my last row to my next (It will be its prev_row)
        MPI_Isend(receive_buff + (column_size * (my_row_size - 1)), 1, game_row, my_next, PREV_ROW_TAG, MPI_COMM_WORLD, &send_request);
        MPI_Request_free(&send_request);

        // Receiving my next_row from my_next
        MPI_Irecv(next_row, 1, game_row, my_next, NEXT_ROW_TAG, MPI_COMM_WORLD, &next_request);

        // Perform possible computation without interacting with other process
        for (int i = 1; i < my_row_size - 1; i++)
            for (int j = 0; j < column_size; j++) {
                int live_count = 0;

                int col_start_index = j - 1;
                int col_end_index = j + 2;

                for (int row = i - 1; row < i + 2; row++)
                    for (int col = col_start_index; col < col_end_index; col++) {
                        if (row == i && col == j)
                            continue;

                        if (isAlive(receive_buff[row * column_size + (col % column_size)]))
                            live_count++;
                    }

                decideFate(receive_buff, result_buff, i * column_size + j, live_count);
            }

        //Wait for some request
        MPI_Request to_wait[] = {prev_request, next_request};
        int handle_index;
        MPI_Waitany(
            2, // array of request's length
            to_wait, // array of requests to wait for
            &handle_index, // Index of handle for operation that completed
            &status
        );

        if(status.MPI_TAG == NEXT_ROW_TAG) { // If the first completed request is next_request
            // perform the logic on next_row
            executeNext(receive_buff, next_row, result_buff, my_row_size, column_size);
            // wait for the prev_row and then perform logic
            waitAndExecutePrev(&prev_request, receive_buff, prev_row, result_buff, column_size);
        } 
        else { // If the first completed request is prev_request
            // perform the logic on prev_row
            executePrev(receive_buff, prev_row, result_buff, column_size);
            // wait for the next_row and then perform logic
            waitAndExecuteNext(&next_request, receive_buff, next_row, result_buff, my_row_size, column_size);
        }

        MPI_Gatherv(result_buff, send_count[rank], game_row, game_matrix, send_count, displacement, game_row, MASTER_NODE, MPI_COMM_WORLD);

        if(is_verbose) {
            if(rank == MASTER_NODE) {
                clearScreen();

                printf("\n\n");
                for (int i = 0; i < row_size; i++) {
                    for (int j = 0; j < column_size; j++)
                        printf("%c", game_matrix[i * column_size + j]);

                    printf("\n");
                }
            }

            usleep(500000);
        }
            
    }

    free(receive_buff);
    free(result_buff);
    free(next_row);
    free(prev_row);

    MPI_Barrier(MPI_COMM_WORLD);

    if(rank == MASTER_NODE) {
        free(game_matrix);
        end_time = MPI_Wtime();

        printf("Time taked = %f\n", end_time - start_time);
    }

    // Finalize the MPI environment.
    MPI_Finalize();

    return 0;
}

void decideFate(char *origin_buff, char *result_buff, int index, int live_count)
{
    if (isAlive(origin_buff[index])) {
        if (live_count < 2)
            die(&result_buff[index]);
        else if (live_count > 3)
            die(&result_buff[index]);
        else
            born(&result_buff[index]);
    }
    else  {
        if (live_count == 3)
            born(&result_buff[index]);
        else
            die(&result_buff[index]);
    }
}

void die(char *cell) {
    *cell = ' ';
}

void born(char *cell) {
    *cell = '#';
}

int isAlive(char cell) {
    return cell == '#';
}

void waitAndExecutePrev(MPI_Request* to_wait, char* origin_buff, char* prev_row, char* result_buff, int column_size) {
    MPI_Wait(to_wait, MPI_STATUS_IGNORE);

    executePrev(origin_buff, prev_row, result_buff, column_size);
}

void executePrev(char* origin_buff, char* prev_row, char* result_buff, int column_size) {
    for (int j = 0; j < column_size; j++) {
        int live_count = 0;

        int col_start_index = j - 1;
        int col_end_index = j + 2;

        for (int row = -1; row < 2; row++)
            for (int col = col_start_index; col < col_end_index; col++) {
                // If I'm on the current item
                if (row == 0 && col == j)
                    continue;

                // If I want to inspect the previous row
                if (row == -1) {
                    if (isAlive(prev_row[col % column_size]))
                        live_count++;
                }
                else {
                    if (isAlive(origin_buff[row * column_size + (col % column_size)]))
                        live_count++;
                }
            }

        decideFate(origin_buff, result_buff, j, live_count);
    }
}

void waitAndExecuteNext(MPI_Request* to_wait, char* origin_buff, char* next_row, char* result_buff, int my_row_size, int column_size) {
    MPI_Wait(to_wait, MPI_STATUS_IGNORE);

    executeNext(origin_buff, next_row, result_buff, my_row_size, column_size);
}

void executeNext(char* origin_buff, char* next_row, char* result_buff, int my_row_size, int column_size) {
    for (int j = 0; j < column_size; j++) {
            int live_count = 0;

            int col_start_index = j - 1;
            int col_end_index = j + 2;

            for (int row = my_row_size - 2; row < my_row_size + 1; row++)
                for (int col = col_start_index; col < col_end_index; col++) {
                    // If I'm on the current item
                    if (row == my_row_size - 1 && col == j)
                        continue;

                    // If I want to inspect the next row
                    if (row == my_row_size) {
                        if (isAlive(next_row[col % column_size]))
                            live_count++;
                    }
                    else {
                        if (isAlive(origin_buff[row * column_size + (col % column_size)]))
                            live_count++;
                    }
                }

            decideFate(origin_buff, result_buff, (my_row_size - 1) * column_size + j, live_count);
        }
}


void clearScreen() {
  //Put the cursor on top-left, so the next generation will be printed over the current one.
  char ANSI_CLS[] = "\x1b[2J";
  char ANSI_HOME[] = "\x1b[H";
  printf("%s%s", ANSI_HOME, ANSI_CLS);
  fflush(stdout);
}

int parseArgs(int argc, char** argv, int* row, int* col, int* iterations, int* verbose) {
    int opt, opt_index;

    static struct option options[] = {
        {"num-row", required_argument, 0, 'r'},
        {"num-col", required_argument, 0, 'c'},
        {"iterations", required_argument, 0, 'i'},
        {"help", no_argument, 0, 'h'},
        {"verbose", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    while((opt = getopt_long(argc, argv, ":r:c:i:v", options, &opt_index)) != -1)  
    {  
        switch(opt)  
        {  
            case 'i':
                *iterations = atoi(optarg);
                break;
            case 'r':
                *row = atoi(optarg);
                break;
            case 'c':
                *col = atoi(optarg);
                break;
            case 'h':
                return 0;
            case 'v':
                *verbose = 1;
                break;
            default:
                return 0;
        }
    }

    if(*row < 3 || *col < 3 || *iterations <= 0)
        return 0;

    return 1;
}

void usage() {
    printf("Usage: GameOfLife -r <integer_greather_then_2> -c <integer_greather_then_2> -i <positive_integer> [-v]\n\n-r, --num-row\t specify the amount of the matrix's row\n-c, --num-col\t specify the amount of the matrix's column\n-i, --iterations\tspecify the number of iterations to perform\n-v, --verbose\tIf the program is in verbose mode, the program prints on stdout with a small pause. Warning! This impact performance! Please, do it only in testing enviroment\n-h, --help\t show this message\n\n");
}