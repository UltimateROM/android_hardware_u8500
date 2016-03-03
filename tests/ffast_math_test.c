#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

int main()
{
	double NaN = sqrt(-1);

	double one = 1;
	double infinity = 1.0 / 0.0;

	if (isnan(NaN))
		printf("NaN = %f\n", NaN);
	else
		printf("NaN != %f\n", NaN);

	if (!isnan(one) && !isinf(one))
		printf("one = %f\n", one);
	else
		prinf("one = inf or NaN");

	
	if (isinf(infinity))
		printf("inf = %f\n", infinity);
	else
		printf("inf != %f\n", infinity);
}
