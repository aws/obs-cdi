#pragma once

static inline enum video_format;

obs_to_cdi_video_format(enum video_format format)
{
	switch (format) {

	case VIDEO_FORMAT_UYVY:
		return kCdiAvmVidYCbCr422;

	}
}

cdi_to_obs_video_format(enum video_format format)
{
	switch (format) {

	case kCdiAvmVidYCbCr422:
		return VIDEO_FORMAT_UYVY;
	}
}