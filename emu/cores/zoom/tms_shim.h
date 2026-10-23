// stand-ins for the MAME helpers the TMS57002 code uses
#ifndef __ZOOM_TMS_SHIM_H__
#define __ZOOM_TMS_SHIM_H__
#define logerror(...)	do {} while(0)
#define popmessage(...)	do {} while(0)
#define fatalerror(...)	abort()
#define ATTR_COLD
#endif
