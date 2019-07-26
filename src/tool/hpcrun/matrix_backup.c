#include <stdlib.h>
#include <stdio.h>

int fs_matrix_size;
int ts_matrix_size;
int as_matrix_size;

int fs_matrix[2000][2000];
int ts_matrix[2000][2000];
int as_matrix[2000][2000];

void dump_fs_matrix()
{
	for(int i = fs_matrix_size; i >= 0; i--) 
	{
		for (int j = 0; j <= fs_matrix_size; j++)
		{
			printf("%d ", fs_matrix[i][j]);
		}
		printf("\n");
	}
}

void dump_ts_matrix()
{
	for(int i = ts_matrix_size; i >= 0; i--) 
	{
		for (int j = 0; j <= ts_matrix_size; j++)
		{
			printf("%d ", ts_matrix[i][j]);
		}
		printf("\n");
	}
}

void dump_as_matrix()
{
	for(int i = as_matrix_size; i >= 0; i--) 
	{
		for (int j = 0; j <= as_matrix_size; j++)
		{
			printf("%d ", as_matrix[i][j]);
		}
		printf("\n");
	}
}

