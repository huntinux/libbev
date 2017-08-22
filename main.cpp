/*
 * main.cpp
 *
 *  Created on: Aug 22, 2017
 *      Author: jinger
 */


#include "net.h"

static void f(struct bufferevent *bev, void *ctx)
{
	printf("own read callback\n");
}

int main()
{
	start_work(10, 9877, f);
	return 0;
}
