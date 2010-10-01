#include "configuration.h"
#include <eegpanel.h>
#include <confuse.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>

// Settings for the allowed "vacabulary" for configuration file
cfg_opt_t filt_opts[] = {
	CFG_BOOL("state", cfg_false, CFGF_NONE),
	CFG_FLOAT("freq", 0.0, CFGF_NONE),
	CFG_END()
};
cfg_opt_t set_opts[] = {
	CFG_STR_LIST("labels", "{}", CFGF_NONE),
	CFG_END()
};

cfg_opt_t opts[] = {
	CFG_SEC("filter", filt_opts, CFGF_MULTI | CFGF_TITLE), 
	CFG_SEC("map", set_opts, CFGF_MULTI | CFGF_TITLE), 
	CFG_FUNC("include", cfg_include),
	CFG_END()
};

void free_label_list(char** labels)
{
	unsigned int i;

	if (!labels)
		return;

	i = 0;
	while (labels[i])
		free(labels[i++]);

	free(labels);
}

int get_filter_settings(cfg_t *cfg, struct FilterSettings* filtset)
{
	if (cfg == NULL)
		return 0;

	filtset->state = cfg_getbool(cfg, "state") == cfg_true ? 1 : 0;
	filtset->freq = cfg_getfloat(cfg, "freq");
	return 0;
}

int get_set_labels(cfg_t *cfg, char*** label_list)
{
	unsigned int nchann, i;
	char *value, **labels;

	if (cfg == NULL)
		return 0;
	
	// Allocate the list
	nchann = cfg_size(cfg, "labels");
	labels = calloc(nchann+1, sizeof(char*));
	if (labels == NULL)
		return 0;
	
	// Allocate and copy each labels
	for (i=0; i<nchann; i++) {
		value = cfg_getnstr(cfg, "labels", i);
		labels[i] = malloc(strlen(value)+1);
		if (!labels[i]) {
			free_label_list(labels);
			return 0;
		}
		strcpy(labels[i], value);
	}

	*label_list = labels;
	return nchann;
}

int read_configuration(struct PanelSettings* settings, const char* eegset, const char* sensorset, const char* filename)
{
	const char* setname;
	int state;
	double freq;
	unsigned int nchann;
	char** labels = NULL;
	cfg_t *cfg = NULL;

	
	setlocale(LC_NUMERIC, "POSIX");

	cfg = cfg_init(opts, 0);
	cfg_parse(cfg, filename);

	// Read the parsed information related to filters
	get_filter_settings(cfg_gettsec(cfg, "filter", "eeglow"), &(settings->filt[EEG_LOWPASS_FLT]));
	get_filter_settings(cfg_gettsec(cfg, "filter", "sensorlow"), &(settings->filt[SENSOR_LOWPASS_FLT]));
	get_filter_settings(cfg_gettsec(cfg, "filter", "eeghigh"), &(settings->filt[EEG_HIGHPASS_FLT]));
	get_filter_settings(cfg_gettsec(cfg, "filter", "sensorhigh"), &(settings->filt[SENSOR_HIGHPASS_FLT]));

	// Read the parsed information related to labels
	nchann = get_set_labels(cfg_gettsec(cfg, "map", eegset), &labels);
	if (labels) {
		settings->eeglabels = (const char**)labels;
		settings->num_eeg = nchann;
	}
	nchann = get_set_labels(cfg_gettsec(cfg, "map", sensorset), &labels);
	if (labels) {
		settings->sensorlabels = (const char**)labels;
		settings->num_sensor = nchann;
	}
	
	cfg_free(cfg);

	return 0;
}

void free_configuration(struct PanelSettings* settings)
{
	free_label_list((char**)settings->eeglabels);
	free_label_list((char**)settings->sensorlabels);
}
