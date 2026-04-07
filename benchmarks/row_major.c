#include <stdio.h>
#include <stdint.h>

#define N 256

int32_t matrix[N][N];

int main(void) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            matrix[i][j] = i + j;
        }
    }

    int32_t sum = 0;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            sum += matrix[i][j];
        }
    }
    printf("row-major sum: %d\n", sum);
}
