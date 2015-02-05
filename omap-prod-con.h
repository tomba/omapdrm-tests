#ifndef _OMAP_PROD_CON_H_
#define _OMAP_PROD_CON_H_

struct shared_output
{
	int output_id;
	int width;
	int height;
	int request_count;
};

struct shared_data
{
	int num_outputs;
	struct shared_output outputs[10];
};

#define SOCKNAME "/tmp/mysock"
#define SHARENAME "/omap-drm-test"

#endif
