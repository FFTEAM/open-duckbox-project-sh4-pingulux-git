#include <config.h>
#if HAVE_TRIPLEDRAGON
#include "../libtriple/cs_api.h"
#elif HAVE_SPARK_HARDWARE
#include "../libspark/cs_api.h"
#elif HAVE_AZBOX_HARDWARE
#include "../azbox/cs_api.h"
#else
#error neither HAVE_TRIPLEDRAGON nor HAVE_SPARK_HARDWARE defined
#endif
