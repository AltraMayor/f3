#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include "libdevs.h"

/* TODO Add parameters. */

int main(void)
{
	struct device *dev;

	dev = create_file_device("xuxu",
		1ULL << 28,	/* Real size.	*/
		1ULL << 31,	/* Fake size.	*/
		29,		/* Wrap.	*/
		9,		/* Block order. */
		false);		/* Keep file?	*/
	assert(dev);

	/* TODO Write blocks. */

	assert(!dev_reset(dev));

	/* TODO Read blocks.  */

	free_device(dev);
}
