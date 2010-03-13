#include <act2demux.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t ns_read;
static struct timespec start;


static unsigned int neeg, nexg;
static ActivetwoSystemInfo info = 
{
	.samplerate = 2048,
	.maxnumber_eeg_channels = 64,
	.maxnumber_sensor_channels = 24,
	.mk_model = 2,
	.speedmode = 4
};

ActivetwoRetCode ActivetwoConnectToSystem(void)
{
	return SUCCESS;
}

void ActivetwoDisconnectFromSystem(void)
{
	
}

ActivetwoSystemInfo ActivetwoGetSystemInfo(void)
{
	return info;
}

ActivetwoRetCode ActivetwoSetDataFormats(EEGformat eeg_format, EXGformat exg_format)
{
	if (eeg_format == A)
		neeg = 32;
	else if (eeg_format == AB)
		neeg = 64;
	else if (eeg_format == ALLEEG)
		neeg = info.maxnumber_eeg_channels;
	else
		return BAD_OPTION;

	if (exg_format == NONE)
		nexg = 0;
	else if (exg_format == STD)
		nexg = 8;
	else if (exg_format == ALLEXG)
		nexg = info.maxnumber_sensor_channels;
	else
		return BAD_OPTION;

	return SUCCESS;
}

ActivetwoRetCode ActivetwoGetLastSamples(unsigned int nSamples, uint32_t* triggers, int32_t* eeg, int32_t* sensors)
{
	return SUCCESS;
}

ActivetwoRetCode ActivetwoStartBufferedAcquisition()
{
	ns_read = 0;
	clock_gettime(CLOCK_MONOTONIC, &start);
	return SUCCESS;
}

void ActivetwoStopBufferedAcquisition()
{
	
}

ActivetwoRetCode ActivetwoGetBufferedSamples(unsigned int nSamples, uint32_t* triggers, int32_t* eeg, int32_t* sensors)
{
	int ns_unread;
	int i, seed;
	uint32_t val;
	for (i=0; i<nSamples; i++) {
		val = ((ns_read + i)/128) % 8;
		triggers[i] = val << 8;
	}
	//memset(triggers, 0, nSamples*sizeof(uint32_t));
	

	memset(eeg, 0, nSamples*neeg*sizeof(int32_t));
	memset(sensors, 0, nSamples*nexg*sizeof(int32_t));

	while (ActiveTwoGetNumUnreadBuffered() < nSamples)
		usleep(10);

	ns_read += nSamples;


	return SUCCESS;
}

int ActiveTwoGetNumUnreadBuffered(void)
{
	struct timespec curr;
	int64_t ns_acq;
	double seconds;

	clock_gettime(CLOCK_MONOTONIC, &curr);

	seconds = (double)(curr.tv_sec - start.tv_sec)
		  + 1e-9*(double)(curr.tv_nsec - start.tv_nsec);

	ns_acq = (int64_t)(seconds * (double)info.samplerate);


	return ns_acq - ns_read;
}
