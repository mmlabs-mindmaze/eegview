#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include <eegpanel.h>

int read_configuration(struct PanelSettings* settings, const char* eegset, const char* sensorset, const char* filename);
void free_configuration(struct PanelSettings* settings);


#endif 
