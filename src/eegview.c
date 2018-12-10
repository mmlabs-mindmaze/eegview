/*
    Copyright (C) 2009-2011  EPFL (Ecole Polytechnique Fédérale de Lausanne)
    Laboratory CNBI (Chair in Non-Invasive Brain-Machine Interface)
    Nicolas Bourdaud <nicolas.bourdaud@gmail.com>

    Copyright (C) 2017-2018  MindMaze SA

    This program is free software: you can redistribute it and/or modify
    modify it under the terms of the version 3 of the GNU General Public
    License as published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <mcpanel.h>
#include <eegdev.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <xdfio.h>
#include <errno.h>
#include <getopt.h>
#include <mmlib.h>
#include <mmerrno.h>

#include "event-tracker.h"


enum {
	REC_PAUSE = 0,
	REC_SAVING,
	REC_RESET_AND_SAVING,
};

struct rectimer_data {
	struct mcp_widget* timerlabel;
	float fs;
	int last_displayed_rectime;
};


/**************************************************************************
 *                                                                        *
 *              Global variables                                          *
 *                                                                        * 
 **************************************************************************/
static const char* uifilename = NULL;
static const char* devstring = NULL;

pthread_t thread_id;
pthread_mutex_t sync_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t file_mtx = PTHREAD_MUTEX_INITIALIZER;
struct eegdev* dev = NULL;
struct xdf* xdf = NULL;
int skip_event_recording = 1;
int run_eeg = 0;
int record_file = 0;
static int reset_record_counter = 0;
#define NSAMPLES	32

struct event_tracker evttrk;

size_t strides[3];
struct grpconf grp[] = {
	{
		.sensortype = EGD_EEG,
		.index = 0,
		.iarray = 0,
		.arr_offset = 0,
		.datatype = EGD_FLOAT
	},
	{
		.sensortype = EGD_SENSOR,
		.index = 0,
		.iarray = 1,
		.arr_offset = 0,
		.datatype = EGD_FLOAT
	},
	{
		.sensortype = EGD_TRIGGER,
		.index = 0,
		.iarray = 2,
		.arr_offset = 0,
		.datatype = EGD_INT32
	}
};
	
static char **labels[3] = {NULL, NULL, NULL};

#define NSCALE 2
static const char* scale_labels[NSCALE] = {"25.0mV", "50.0mV"};
static const float scale_values[NSCALE] = {25.0e3, 50.0e3};

#define NTAB 4
static struct panel_tabconf tabconf[NTAB] = {
	{.type = TABTYPE_SCOPE, .name = "EEG"},
	{.type = TABTYPE_SPECTRUM, .name = "EEG spectrum"},
	{.type = TABTYPE_BARGRAPH, .name = "Offsets", .nscales = NSCALE,
	 .sclabels = scale_labels, .scales = scale_values},
	{.type = TABTYPE_SCOPE, .name = "Sensors"}
};

static int StopRecording(void* user_data);
/**************************************************************************
 *                                                                        *
 *              Error message helper functions                            *
 *                                                                        * 
 **************************************************************************/
const char* get_acq_msg(int error)
{
	return strerror(error);
}

static char bdffile_message[128];


/**************************************************************************
 *                                                                        *
 *              recording time display                                    *
 *                                                                        *
 **************************************************************************/
/**
 * rectimer_data_init() - initialize data for updating recorded time label
 * @data:       rectimer_data structure to initialize
 * @panel:      mcpanel instance that should contain the label
 * @fs:         sampling frequency of the recorded signal
 */
static
void rectimer_data_init(struct rectimer_data* data, mcpanel* panel, float fs)
{
	*data = (struct rectimer_data) {
		.fs = fs,
		.timerlabel = mcp_get_widget(panel, "file_length_label"),
		.last_displayed_rectime = 0,
	};
}


/**
 * rectimer_data_update() - update recorded time label in GUI
 * @data:       initialized rectimer_data structure
 * @total_rec:  number of sample since file started to be recorded
 */
static
void rectimer_data_update(struct rectimer_data* data, ssize_t total_rec)
{
	int rectime;
	char text_label[32];

	rectime = total_rec / data->fs;
	if (rectime == data->last_displayed_rectime)
		return;

	sprintf(text_label, "%d", rectime);
	mcp_widget_set_label(data->timerlabel, text_label);

	data->last_displayed_rectime = rectime;
}


/**************************************************************************
 *                                                                        *
 *              Acquition system callbacks                                *
 *                                                                        * 
 **************************************************************************/
static
void free_labels(void)
{
	unsigned int igrp, i;

	for (igrp=0; igrp<3; igrp++) {
		if (labels[igrp] == NULL)
			continue;
		for (i=0; i<grp[igrp].nch; i++) 
			free(labels[igrp][i]);

		free(labels[igrp]);
		labels[igrp] = NULL;
	}
}

static
int get_labels_from_device(void)
{
	unsigned int i, igrp;
	int type;

	// Allocate and copy each labels
	for (igrp=0; igrp<3; igrp++) {
		labels[igrp] = malloc(grp[igrp].nch * sizeof(char*));
		type = grp[igrp].sensortype;
		for (i=0; i<grp[igrp].nch; i++) {
			labels[igrp][i] = malloc(32);
			egd_channel_info(dev, type, i,
			               EGD_LABEL, labels[igrp][i], EGD_EOL);
		}
	}
	return 0;
}


static
void* display_bdf_error(void* arg)
{
	mcpanel* pan = arg;
	mcp_popup_message(pan, bdffile_message);
	return NULL;
}

static
int device_connection(void)
{
	int retval;
	if (!(dev = egd_open(devstring)))
		return errno;

	// Set the number of channels of the "All channels" values
	grp[0].nch = egd_get_numch(dev, EGD_EEG);
	grp[1].nch = egd_get_numch(dev, EGD_SENSOR);
	grp[2].nch = egd_get_numch(dev, EGD_TRIGGER);

	strides[0] = grp[0].nch * sizeof(float);
	strides[1] = grp[1].nch * sizeof(float);
	strides[2] = grp[2].nch * sizeof(int32_t);

	get_labels_from_device();

	// Set the acquisition according to the settings
	if (egd_acq_setup(dev, 3, strides, 3, grp)) {
		retval = errno;
		egd_close(dev);
		return retval;
	}

	return 0;
}


static
int device_disconnection(void)
{
	free_labels();
	egd_close(dev);
	return 0;
}


/**
 * record_event() - add events to xdffile
 * @evt_stk:    stack of event to store in file
 * @fs:         sampling freqency of acquisition
 * @diff_idx:   index of acquired sample when recording started
 */
static
int record_event(struct event_stack* evt_stk, float fs, int diff_idx)
{
	int e, evttype;
	double onset;

	if (skip_event_recording)
		return 0;

	for (e = 0; e < evt_stk->nevent; e++) {
		// Get XDF event type
		evttype = xdf_add_evttype(xdf, evt_stk->events[e].type, NULL);
		if (evttype == -1) {
			mm_raise_from_errno("xdf_add_evttype() failed");
			return -1;
		}

		// Compute onset in floating point (in seconds) since
		// begining of recording
		onset = (evt_stk->events[e].pos - diff_idx) / fs;
		if (xdf_add_event(xdf, evttype, onset, 0.0f)) {
			mm_raise_from_errno("xdf_add_event(..., %d, ...) failed", evttype);
			return -1;
		}
	}
	return 0;
}

// EEG acquisition thread
static
void* reading_thread(void* arg)
{
	float *eeg, *exg;
	int32_t *tri;
	mcpanel* panel = arg;
	unsigned int neeg, nexg, ntri;
	int run_acq, error, saving = 0;
	int nsread, total_rec, total_read, rec_start;
	float fs;
	struct rectimer_data rectimer;
	struct event_tracker* trk = &evttrk;
	struct event_stack* evt_stk;

	fs = egd_get_cap(dev, EGD_CAP_FS, NULL);
	rectimer_data_init(&rectimer, panel, fs);

	neeg = grp[0].nch;
	nexg = grp[1].nch;
	ntri = grp[2].nch;

	eeg = neeg ? calloc(neeg*NSAMPLES, sizeof(*eeg)) : NULL;
	exg = nexg ? calloc(nexg*NSAMPLES, sizeof(*exg)) : NULL;
	tri = ntri ? calloc(ntri*NSAMPLES, sizeof(*tri)) : NULL;

	egd_start(dev);
	total_read = 0;
	total_rec = 0;
	rec_start = 0;

	while (1) {

		// update control flags
		pthread_mutex_lock(&sync_mtx);
		run_acq = run_eeg;
		if (saving != record_file) {
			// report write status back
			if (record_file != REC_PAUSE)
				pthread_mutex_lock(&file_mtx);
			else
				pthread_mutex_unlock(&file_mtx);
			saving = record_file;

			if (saving == REC_RESET_AND_SAVING) {
				total_rec = 0;
				rec_start = total_read;
			}
		}
		pthread_mutex_unlock(&sync_mtx);

		// Check the stop acquisition flag
		if (!run_acq)
			break;

		// Get data from the system
		nsread = egd_get_data(dev, NSAMPLES, eeg, exg, tri);
		if (nsread < 0) {
			error = errno;			
			mcp_notify(panel, DISCONNECTED);
			mcp_popup_message(panel, get_acq_msg(error));
			break;
		}
		total_read += nsread;
		event_tracker_update_ns_read(trk, total_read);
		evt_stk = event_tracker_swap_eventstack(trk);

		// Write samples on file
		if (saving != REC_PAUSE) {
			if (xdf_write(xdf, nsread, eeg, exg, tri) < 0) {
				pthread_attr_t attr;
				pthread_t thid;
				sprintf(bdffile_message,"XDF Error: %s",strerror(errno));
			
				// Stop recording
				saving = 0;
				pthread_mutex_unlock(&file_mtx);
				StopRecording(NULL);

				// Pop up message
				pthread_attr_init(&attr);
				pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
				pthread_create(&thid, &attr, display_bdf_error, panel);
				pthread_attr_destroy(&attr);
			}
			record_event(evt_stk, fs, rec_start);

			total_rec += nsread;

			// display how long we are recording
			rectimer_data_update(&rectimer, total_rec);
		}

		mcp_add_events(panel, 0, evt_stk->nevent, evt_stk->events);
		mcp_add_samples(panel, 0, nsread, eeg);
		mcp_add_samples(panel, 1, nsread, eeg);
		mcp_add_samples(panel, 2, nsread, eeg);
		mcp_add_samples(panel, 3, nsread, exg);
		mcp_add_triggers(panel, nsread, (const uint32_t*)tri);

	}

	if (saving)
		pthread_mutex_unlock(&file_mtx);

	egd_stop(dev);

	free(eeg);
	free(exg);
	free(tri);

	return 0;
}


// Connection to the system
static
int Connect(mcpanel* panel)
{
	int retval;
	const char*** clabels;
	float fs;
	unsigned int ntri;

	retval = device_connection();
	if (retval)
		return retval;

	fs = egd_get_cap(dev, EGD_CAP_FS, NULL);
	clabels = (const char***)labels;

	// Retrieve the number of trigger channels
	ntri = grp[2].nch;

	// Setup the panel with the settings
	mcp_define_tab_input(panel, 0, grp[0].nch, fs, clabels[0]);
	mcp_define_tab_input(panel, 1, grp[0].nch, fs, clabels[0]);
	mcp_define_tab_input(panel, 2, grp[0].nch, fs, clabels[0]);
	mcp_define_tab_input(panel, 3, grp[1].nch, fs, clabels[1]);
	mcp_define_trigg_input(panel, 16, ntri, fs, clabels[2]);

	pthread_mutex_lock(&sync_mtx);
	run_eeg = 1;
	pthread_mutex_unlock(&sync_mtx);
	pthread_create(&thread_id, NULL, reading_thread, panel);

	// Network event connection and reception
	event_tracker_init(&evttrk, fs);

	return 0;
}


static
int Disconnect(mcpanel* panel)
{
	(void)panel;

	StopRecording(NULL);

	pthread_mutex_lock(&sync_mtx);
	run_eeg = 0;
	pthread_mutex_unlock(&sync_mtx);

	pthread_join(thread_id, NULL);
	device_disconnection();

	event_tracker_deinit(&evttrk);

	return 0;
}


static
int SystemConnection(int start, void* user_data)
{
	mcpanel* panel = user_data;
	int retval;

	retval = start ? Connect(panel) : Disconnect(panel);
	if (retval) {
		mcp_popup_message(panel, get_acq_msg(retval));
		return 0;
	}

	return 1;
}


static char devinfo[1024];

static
void on_device_info(int id, void* data)
{
	(void)id;
	unsigned int sampling_freq, eeg_nmax, sensor_nmax, trigger_nmax;
	char *device_type, *device_id;
	char prefiltering[128];
	mcpanel* panel = data;

	if (!run_eeg)
		return;

	egd_get_cap(dev, EGD_CAP_DEVTYPE, &device_type);
	egd_get_cap(dev, EGD_CAP_DEVID, &device_id);
	egd_get_cap(dev, EGD_CAP_FS, &sampling_freq);
	eeg_nmax = egd_get_numch(dev, EGD_EEG);
	sensor_nmax = egd_get_numch(dev, EGD_SENSOR);
	trigger_nmax = egd_get_numch(dev, EGD_TRIGGER);
	egd_channel_info(dev, EGD_EEG, 0,
			EGD_PREFILTERING, prefiltering, EGD_EOL);
	
	snprintf(devinfo, sizeof(devinfo)-1,
	       "system info:\n\n"
	       "device type: %s\n"
	       "device model: %s\n"
	       "sampling frequency: %u Hz\n"
	       "num EEG channels: %u\n"
	       "num sensor channels: %u\n"
	       "num trigger channels: %u\n"
	       "prefiltering: %s\n",
	       device_type, device_id, sampling_freq,
	       eeg_nmax, sensor_nmax, trigger_nmax, prefiltering);
	
	mcp_popup_message(panel, devinfo);	
}

/**************************************************************************
 *                                                                        *
 *              File recording callbacks                                  *
 *                                                                        * 
 **************************************************************************/
static
int setup_xdf_channel_group(int igrp)
{
	char label[32], transducter[128], unit[16], filtering[128];
	double mm[2];
	unsigned int j;
	int isint;
	struct xdfch* ch;

	egd_channel_info(dev, grp[igrp].sensortype, 0,
			 EGD_UNIT, unit,
			 EGD_TRANSDUCTER, transducter,
			 EGD_PREFILTERING, filtering,
			 EGD_MM_D, mm,
			 EGD_ISINT, &isint,
			 EGD_EOL);

	xdf_set_conf(xdf, 
	               XDF_CF_ARRINDEX, igrp,
		       XDF_CF_ARROFFSET, 0,
		       XDF_CF_ARRDIGITAL, 0,
		       XDF_CF_ARRTYPE, isint ? XDFINT32 : XDFFLOAT,
		       XDF_CF_PMIN, mm[0],
		       XDF_CF_PMAX, mm[1],
		       XDF_CF_TRANSDUCTER, transducter,
	               XDF_CF_PREFILTERING, filtering,
		       XDF_CF_UNIT, unit,
		       XDF_NOF);

	for (j = 0; j < grp[igrp].nch; j++) {
		egd_channel_info(dev, grp[igrp].sensortype, j,
		                 EGD_LABEL, label, EGD_EOL);

		// Add the channel to the BDF
		if ((ch = xdf_add_channel(xdf, label)) == NULL)
			return -1;
	}
	return 0;
}

static
void create_file(mcpanel* panel)
{
	const char *fileext, *dot;
	char *filename;

	xdf = NULL;

	filename = mcp_open_filename_dialog(panel,
	                              "GDF files|*.gdf|*.GDF||BDF files|*.bdf|*.BDF||Any files|*");

	// Check that user hasn't pressed cancel
	if (filename == NULL)
		return ;

	// Create the BDF/GDF file
	dot = strrchr(filename, '.');
	if (!dot || dot == filename) {
		fprintf(stderr, "No file extension has been provided! Defaulting to GDF\n");
		fileext = "gdf";
	} else {
		fileext = dot + 1;
	}

	if (mmstrcasecmp(fileext, "bdf")==0) {
		xdf = xdf_open(filename, XDF_WRITE, XDF_BDF);
	} else if (mmstrcasecmp(fileext, "gdf")==0) {
		xdf = xdf_open(filename, XDF_WRITE, XDF_GDF2);
	} else {
		fprintf(stderr, "File extension should be either BDF or GDF! Defaulting to GDF\n");
		xdf = xdf_open(filename, XDF_WRITE, XDF_GDF2);
	}
}

static
int SetupRecording(void *user_data)
{
	mcpanel *panel = user_data;
	unsigned int j;
	int fileformat = -1;
	int fs = egd_get_cap(dev, EGD_CAP_FS, NULL);

	create_file(panel);
	if (!xdf) 
		goto abort;

	// Configuration file genral header
	xdf_set_conf(xdf,
	             XDF_F_REC_DURATION, 1.0,
	             XDF_F_REC_NSAMPLE, fs,
		     XDF_NOF);

	// Set up the channels
	for (j=0; j<3; j++)	
		if (setup_xdf_channel_group(j))
			goto abort;

	// Make the file ready for recording
	xdf_define_arrays(xdf, 3, strides);
	if (xdf_prepare_transfer(xdf))
		goto abort;

	//Store file type for later use
	xdf_get_conf(xdf, XDF_F_FILEFMT, &fileformat, XDF_NOF);
	skip_event_recording = (fileformat != XDF_GDF2);
	reset_record_counter = 1;
	return 1;
	
abort:
	sprintf(bdffile_message,"XDF Error: %s",strerror(errno));
	mcp_popup_message(panel, bdffile_message);
	xdf_close(xdf);
	return 0;
}

static
int StopRecording(void* user_data)
{
	(void)user_data;

	pthread_mutex_lock(&sync_mtx);
	record_file = 0;
	pthread_mutex_unlock(&sync_mtx);
	
	pthread_mutex_lock(&file_mtx);
	xdf_close(xdf);
	xdf = NULL;
	pthread_mutex_unlock(&file_mtx);

	return 1;
}


static
int ToggleRecording(int start, void* user_data)
{
	(void)user_data;

	pthread_mutex_lock(&sync_mtx);

	record_file = start ? REC_SAVING : REC_PAUSE;

	// If it is the first toggle_recording after recording setup, we
	// must reset the counters.
	if (reset_record_counter && start) {
		record_file = REC_RESET_AND_SAVING;
		reset_record_counter = 0;
	}

	pthread_mutex_unlock(&sync_mtx);

	return 1;
}



/**************************************************************************
 *                                                                        *
 *              Initialization of the application                         *
 *                                                                        * 
 **************************************************************************/
enum option_index {
	UIFILE,
	DEVSTRING,
	SOFTWAREVERSION,
	HELP,
	NUM_OPTS
};

static struct option opt_str[] = {
	[UIFILE] = {"ui-file", 1, NULL, 0},
	[DEVSTRING] = {"device", 1, NULL, 0},
	[SOFTWAREVERSION] = {"version", 0, NULL, 0},
	[HELP] = {"help", 0, NULL, 'h'}

};


static void print_usage(const char* cmd)
{
	fprintf(stdout,
"Usage: %s [GTK+ OPTIONS...]\n"
"            [--device=DEVSTRING] [--ui-file=FILE]\n"
"            [--version] [--help | -h]\n",
               cmd);
	
}

static void print_version(void)
{
	printf("%s: version %s build on\n\t%s\n\t%s\n",
	       PACKAGE_NAME, 
	       PACKAGE_VERSION,
	       egd_get_string(),
	       xdf_get_string());
}

static 
int process_options(int argc, char* argv[])
{
	int c;
	int option_index = 0;

	while (1) {
		c = getopt_long(argc, argv, "h", opt_str, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 0:
			if (option_index == UIFILE)
				uifilename = optarg;
			else if (option_index == DEVSTRING)
				devstring = optarg;
			else if (option_index == SOFTWAREVERSION)
				print_version();
			break;

		case 'h':
			print_usage(argv[0]);
			return 1;

		case '?':
			print_usage(argv[0]);
			return -1;
		}
	}

	return 0;
}


int main(int argc, char* argv[])
{
	mcpanel* panel = NULL;
	int retval = 0, retcode = EXIT_FAILURE;
	struct panel_button custom_button = {
		.label = "device info",
		.id = 0,
		.callback = on_device_info
	};
	struct PanelCb cb = {
		.user_data = NULL,
		.system_connection = SystemConnection,
		.setup_recording = SetupRecording,
		.stop_recording = StopRecording,
		.toggle_recording = ToggleRecording,
		.nbutton = 1,
		.custom_button = &custom_button,
		.confname = PACKAGE_NAME
	};

	// Process command line options
	mcp_init_lib(&argc, &argv);
	retval = process_options(argc, argv);
	if (retval)
		return (retval > 0) ? 0 : -retval;
	

	panel = mcp_create(uifilename, &cb, NTAB, tabconf);
	if (!panel) {
		fprintf(stderr,"error at the creation of the panel\n");
		goto exit;
	}
	
	// Run the panel
	mcp_show(panel, 1);
	mcp_run(panel, 0);
	if (run_eeg)
		Disconnect(panel);

	mcp_destroy(panel);
	retcode = EXIT_SUCCESS;

exit:
	return retcode;
}

