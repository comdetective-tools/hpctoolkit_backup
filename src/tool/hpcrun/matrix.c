#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "matrix.h"

int fs_matrix_size;
int ts_matrix_size;
int as_matrix_size;

int fs_core_matrix_size;
int ts_core_matrix_size;
int as_core_matrix_size;

int max_consecutive_count = 0;

int HASHTABLESIZE;

double fs_matrix[2000][2000];
double ts_matrix[2000][2000];
double as_matrix[2000][2000];

double fs_core_matrix[2000][2000];
double ts_core_matrix[2000][2000];
double as_core_matrix[2000][2000];

long number_of_traps;

long global_store_sampling_period;
long global_load_sampling_period;

// before
__thread long number_of_sample = 0;
__thread long number_of_load_sample = 0;
__thread long number_of_store_sample = 0;
__thread long number_of_load_store_sample = 0;
__thread long number_of_load_store_sample_all_loads = 0;
__thread long number_of_load_store_sample_all_stores = 0;
__thread long number_of_arming = 0;
__thread long number_of_caught_traps = 0;
__thread long number_of_caught_read_traps = 0;
__thread long number_of_caught_write_traps = 0;
__thread long number_of_caught_read_write_traps = 0;
__thread long number_of_bulletin_board_updates_before = 0;
__thread long number_of_bulletin_board_updates = 0;
__thread long number_of_residues = 0;
// after

int consecutive_access_count_array[50];

int consecutive_wasted_trap_array[50];

void dump_fs_matrix()
{
	FILE * fp;
	char file_name[50]; 
	sprintf(file_name, "%ldfs_matrix.csv", (long) clock()); 
	fp = fopen (file_name, "w+");
	//printf("fs_matrix_size: %d\n", fs_matrix_size);
	double total= 0;
	for(int i = fs_matrix_size; i >= 0; i--) 
	{
		for (int j = 0; j <= fs_matrix_size; j++)
		{
			if(j < fs_matrix_size) {
                                fprintf(fp, "%0.2lf,", fs_matrix[i][j] + fs_matrix[j][i]);
                                total += fs_matrix[i][j];
                                //printf("%0.2lf,", fs_matrix[i][j] + fs_matrix[j][i]);
                        } else {
                                fprintf(fp, "%0.2lf", fs_matrix[i][j] + fs_matrix[j][i]);
                                total += fs_matrix[i][j];
                                //printf("%0.2lf", fs_matrix[i][j] + fs_matrix[j][i]);
                        }
		}
		fprintf(fp,"\n");
		//printf("\n");
	}
	fclose(fp);
	printf("total false sharing volume: %0.2lf\n", total);
}

void dump_fs_core_matrix()
{
        FILE * fp;
        char file_name[50];
	sprintf(file_name, "%ldfs_core_matrix.csv", (long) clock());
        fp = fopen (file_name, "w+");
        //printf("fs_core_matrix_size: %d\n", fs_core_matrix_size);
        double total= 0;
        for(int i = fs_core_matrix_size; i >= 0; i--)
        {
                for (int j = 0; j <= fs_core_matrix_size; j++)
                {
			if(j < fs_core_matrix_size) {
                                fprintf(fp, "%0.2lf,", fs_core_matrix[i][j] + fs_core_matrix[j][i]);
                                total += fs_core_matrix[i][j];
                                //printf("%0.2lf,", fs_core_matrix[i][j] + fs_core_matrix[j][i]);
                        } else {
                                fprintf(fp, "%0.2lf", fs_core_matrix[i][j] + fs_core_matrix[j][i]);
                                total += fs_core_matrix[i][j];
                                //printf("%0.2lf", fs_core_matrix[i][j] + fs_core_matrix[j][i]);
                        }
                }
                fprintf(fp,"\n");
                //printf("\n");
        }
        fclose(fp);
        printf("total inter core false sharing volume: %0.2lf\n", total);
}

void dump_ts_matrix()
{
	FILE * fp;
	char file_name[50];
	sprintf(file_name, "%ldts_matrix.csv", (long) clock());
	fp = fopen (file_name, "w+");
	//printf("ts_matrix_size: %d\n", ts_matrix_size);
	double total = 0;
	for(int i = ts_matrix_size; i >= 0; i--) 
	{
		for (int j = 0; j <= ts_matrix_size; j++)
		{
			if(j < ts_matrix_size) {
                                fprintf(fp, "%0.2lf,", ts_matrix[i][j] + ts_matrix[j][i]);
                                total += ts_matrix[i][j];
                                //printf("%0.2lf,", ts_matrix[i][j] + ts_matrix[j][i]);
                        } else {
                                fprintf(fp, "%0.2lf", ts_matrix[i][j] + ts_matrix[j][i]);
                                total += ts_matrix[i][j];
                                //printf("%0.2lf", ts_matrix[i][j] + ts_matrix[j][i]);
                        }
		}
		fprintf(fp,"\n");
		//printf("\n");
	}
	fclose(fp);
	printf("total true sharing volume: %0.2lf\n", total);
}

void dump_ts_core_matrix()
{
        FILE * fp;
        char file_name[50];
	sprintf(file_name, "%ldts_core_matrix.csv", (long) clock());
        fp = fopen (file_name, "w+");
        //printf("ts_core_matrix_size: %d\n", ts_core_matrix_size);
        double total = 0;
        for(int i = ts_core_matrix_size; i >= 0; i--)
        {
                for (int j = 0; j <= ts_core_matrix_size; j++)
                {
			if(j < ts_core_matrix_size) {
                                fprintf(fp, "%0.2lf,", ts_core_matrix[i][j] + ts_core_matrix[j][i]);
                                total += ts_core_matrix[i][j];
                                //printf("%0.2lf,", ts_core_matrix[i][j] + ts_core_matrix[j][i]);
                        } else {
                                fprintf(fp, "%0.2lf", ts_core_matrix[i][j] + ts_core_matrix[j][i]);
                                total += ts_core_matrix[i][j];
                                //printf("%0.2lf", ts_core_matrix[i][j] + ts_core_matrix[j][i]);
                        }
                }
                fprintf(fp,"\n");
                //printf("\n");
        }
        fclose(fp);
        printf("total inter core true sharing volume: %0.2lf\n", total);
}

void dump_as_matrix()
{
	FILE * fp;
	char file_name[50];
	long timeprint = (long) clock();
	sprintf(file_name, "%ldas_matrix.csv", timeprint);
	fp = fopen (file_name, "w+");
	//printf("as_matrix_size: %d\n", as_matrix_size);
	double total = 0;
	//printf("all sharing matrix:\n");
	for(int i = as_matrix_size; i >= 0; i--)
	{
		for (int j = 0; j <= as_matrix_size; j++)
		{
			if(j < as_matrix_size) {
				fprintf(fp, "%0.2lf,", as_matrix[i][j] + as_matrix[j][i]);
				total += as_matrix[i][j];
				//printf("%0.2lf,", as_matrix[i][j] + as_matrix[j][i]);
			} else {
				fprintf(fp, "%0.2lf", as_matrix[i][j] + as_matrix[j][i]);
                                total += as_matrix[i][j];
                                //printf("%0.2lf", as_matrix[i][j] + as_matrix[j][i]);
			}
		}
		fprintf(fp,"\n");
		//printf("\n");
	}
	printf("total communication volume: %0.2lf, timeprint: %ld\n", total, timeprint);
	fclose(fp);
}

void dump_as_core_matrix()
{
        FILE * fp;
        char file_name[50];
	long timeprint = (long) clock();
	sprintf(file_name, "%ldas_core_matrix.csv", timeprint);
        fp = fopen (file_name, "w+");
        //printf("as_matrix_size: %d\n", as_matrix_size);
        double total = 0;
        //printf("all sharing matrix:\n");
        for(int i = as_core_matrix_size; i >= 0; i--)
        {
                for (int j = 0; j <= as_core_matrix_size; j++)
                {
                        if(j < as_core_matrix_size) {
                                fprintf(fp, "%0.2lf,", as_core_matrix[i][j] + as_core_matrix[j][i]);
                                total += as_core_matrix[i][j];
                                //printf("%0.2lf,", as_core_matrix[i][j] + as_core_matrix[j][i]);
                        } else {
                                fprintf(fp, "%0.2lf", as_core_matrix[i][j] + as_core_matrix[j][i]);
                                total += as_core_matrix[i][j];
                                //printf("%0.2lf", as_core_matrix[i][j] + as_core_matrix[j][i]);
                        }
                }
                fprintf(fp,"\n");
                //printf("\n");
        }
        printf("total inter core communication volume: %0.2lf, timeprint: %ld\n", total, timeprint);
	printf("cache line transfer count: %0.2lf\n", total);
	printf("cache line transfer count (Millions): %0.2lf\n", total/(1000000));
	printf("cache line transfer size (GBytes): %0.2lf\n", total*64/(1024*1024*1024));
        fclose(fp);
}
