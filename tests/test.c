/* standard C library headers required */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* the following is optional depending on the timing function used */
#include <time.h>

int main(int argc, char *argv[])
{
	long double x = 0.5;
	long double y = 0.5;

	double X, Y, T;
	int I, N7 = 2;

	//printf("x=%12.4e\n", x);
	//printf("y=%12.4e\n", y);

	printf("x=%f\n", x);
	printf("y=%f\n", y);

	printf("sin(x * M_PI) = %f\n", sin(x * M_PI));
	printf("cos(x * M_PI) = %f\n", cos(x * M_PI));

        X = 0.5;
        Y = 0.5;
        for (I = 1; I <= N7; I++) {
                printf("X=%12.4e\n", X);
                printf("Y=%12.4e\n", Y);

                X=cos(X+Y)+cos(X-Y)-1.0;
                Y=cos(X+Y)+cos(X-Y)-1.0;
                printf("X=cos(X+Y)+cos(X-Y)-1.0=%12.4e\n", X);
                printf("Y=cos(X+Y)+cos(X-Y)-1.0=%12.4e\n", Y);

                //X = T * DATAN(T2*DSIN(X)*cos(X)/(cos(X+Y)+cos(X-Y)-1.0));
                //Y = T * DATAN(T2*DSIN(Y)*cos(Y)/(cos(X+Y)+cos(X-Y)-1.0));
        }

/*
	x = cos(x+y)+cos(x-y)-1.0;
	y = cos(x+y)+cos(x-y)-1.0;
	printf("x=cos(x+y)+cos(x-y)-1.0 = %f\n", x);
	printf("y=cos(x+y)+cos(x-y)-1.0 = %f\n", y);
*/

	return 0;
}
