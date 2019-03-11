/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * ipu3.cpp - Pipeline handler for Intel IPU3
 */

#include <iomanip>
#include <memory>
#include <vector>

#include <linux/media-bus-format.h>

#include <libcamera/camera.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>

#include "device_enumerator.h"
#include "log.h"
#include "media_device.h"
#include "pipeline_handler.h"
#include "utils.h"
#include "v4l2_device.h"
#include "v4l2_subdevice.h"

namespace libcamera {

LOG_DEFINE_CATEGORY(IPU3)

class ImgUDevice
{
public:
	static constexpr unsigned int PAD_INPUT = 0;
	static constexpr unsigned int PAD_OUTPUT = 2;
	static constexpr unsigned int PAD_VF = 3;
	static constexpr unsigned int PAD_STAT = 4;

	/* ImgU output descriptor: group data specific to an ImgU output. */
	struct ImgUOutput {
		V4L2Device *dev;
		unsigned int pad;
		std::string name;
	};

	ImgUDevice()
		: imgu_(nullptr), input_(nullptr)
	{
		output_.dev = nullptr;
		viewfinder_.dev = nullptr;
		stat_.dev = nullptr;
	}

	~ImgUDevice()
	{
		delete imgu_;
		delete input_;
		delete output_.dev;
		delete viewfinder_.dev;
		delete stat_.dev;
	}

	int init(MediaDevice *media, unsigned int index);
	int configureInput(const StreamConfiguration &config,
			   V4L2DeviceFormat *inputFormat);
	int configureOutput(ImgUOutput *output,
			    const StreamConfiguration &config);

	unsigned int index_;
	std::string name_;
	MediaDevice *media_;

	V4L2Subdevice *imgu_;
	V4L2Device *input_;
	ImgUOutput output_;
	ImgUOutput viewfinder_;
	ImgUOutput stat_;
	/* \todo Add param video device for 3A tuning */
};

class CIO2Device
{
public:
	CIO2Device()
		: output_(nullptr), csi2_(nullptr), sensor_(nullptr)
	{
	}

	~CIO2Device()
	{
		delete output_;
		delete csi2_;
		delete sensor_;
	}

	int init(const MediaDevice *media, unsigned int index);
	int configure(const StreamConfiguration &config,
		      V4L2DeviceFormat *outputFormat);

	static int mediaBusToFormat(unsigned int code);

	V4L2Device *output_;
	V4L2Subdevice *csi2_;
	V4L2Subdevice *sensor_;

	/* Maximum sizes and the mbus code used to produce them. */
	unsigned int mbusCode_;
	Size maxSize_;
};

class PipelineHandlerIPU3 : public PipelineHandler
{
public:
	PipelineHandlerIPU3(CameraManager *manager);
	~PipelineHandlerIPU3();

	std::map<Stream *, StreamConfiguration>
	streamConfiguration(Camera *camera,
			    std::set<Stream *> &streams) override;
	int configureStreams(Camera *camera,
			     std::map<Stream *, StreamConfiguration> &config) override;

	int allocateBuffers(Camera *camera, Stream *stream) override;
	int freeBuffers(Camera *camera, Stream *stream) override;

	int start(Camera *camera) override;
	void stop(Camera *camera) override;

	int queueRequest(Camera *camera, Request *request) override;

	bool match(DeviceEnumerator *enumerator);

private:
	class IPU3CameraData : public CameraData
	{
	public:
		IPU3CameraData(PipelineHandler *pipe)
			: CameraData(pipe)
		{
		}

		void bufferReady(Buffer *buffer);

		CIO2Device cio2_;
		ImgUDevice *imgu_;

		Stream stream_;
	};

	IPU3CameraData *cameraData(const Camera *camera)
	{
		return static_cast<IPU3CameraData *>(
			PipelineHandler::cameraData(camera));
	}

	int registerCameras();

	ImgUDevice imgu0_;
	ImgUDevice imgu1_;
	std::shared_ptr<MediaDevice> cio2MediaDev_;
	std::shared_ptr<MediaDevice> imguMediaDev_;
};

PipelineHandlerIPU3::PipelineHandlerIPU3(CameraManager *manager)
	: PipelineHandler(manager), cio2MediaDev_(nullptr), imguMediaDev_(nullptr)
{
}

PipelineHandlerIPU3::~PipelineHandlerIPU3()
{
	if (cio2MediaDev_)
		cio2MediaDev_->release();

	if (imguMediaDev_)
		imguMediaDev_->release();
}

std::map<Stream *, StreamConfiguration>
PipelineHandlerIPU3::streamConfiguration(Camera *camera,
					 std::set<Stream *> &streams)
{
	IPU3CameraData *data = cameraData(camera);
	std::map<Stream *, StreamConfiguration> configs;
	V4L2SubdeviceFormat format = {};

	/*
	 * FIXME: As of now, return the image format reported by the sensor.
	 * In future good defaults should be provided for each stream.
	 */
	if (data->cio2_.sensor_->getFormat(0, &format)) {
		LOG(IPU3, Error) << "Failed to create stream configurations";
		return configs;
	}

	StreamConfiguration config = {};
	config.width = format.width;
	config.height = format.height;
	config.pixelFormat = V4L2_PIX_FMT_IPU3_SGRBG10;
	config.bufferCount = 4;

	configs[&data->stream_] = config;

	return configs;
}

int PipelineHandlerIPU3::configureStreams(Camera *camera,
					  std::map<Stream *, StreamConfiguration> &config)
{
	IPU3CameraData *data = cameraData(camera);
	const StreamConfiguration &cfg = config[&data->stream_];
	CIO2Device *cio2 = &data->cio2_;
	ImgUDevice *imgu = data->imgu_;
	int ret;

	LOG(IPU3, Info)
		<< "Requested image format " << cfg.width << "x"
		<< cfg.height << "-0x" << std::hex << std::setfill('0')
		<< std::setw(8) << cfg.pixelFormat << " on camera '"
		<< camera->name() << "'";

	/*
	 * Verify that the requested size respects the IPU3 alignement
	 * requirements (the image width shall be a multiple of 8 pixels and
	 * its height a multiple of 4 pixels) and the camera maximum sizes.
	 *
	 * \todo: consider the BDS scaling factor requirements:
	 * "the downscaling factor must be an integer value multiple of 1/32"
	 */
	if (cfg.width % 8 || cfg.height % 4) {
		LOG(IPU3, Error) << "Invalid stream size: bad alignment";
		return -EINVAL;
	}

	if (cfg.width > cio2->maxSize_.width ||
	    cfg.height > cio2->maxSize_.height) {
		LOG(IPU3, Error)
			<< "Invalid stream size: larger than sensor resolution";
		return -EINVAL;
	}

	/*
	 * Pass the requested stream size to the CIO2 unit and get back the
	 * adjusted format to be propagated to the ImgU output devices.
	 */
	V4L2DeviceFormat cio2Format = {};
	ret = cio2->configure(cfg, &cio2Format);
	if (ret)
		return ret;

	ret = imgu->configureInput(cfg, &cio2Format);
	if (ret)
		return ret;

	/* Apply the format to the ImgU output, viewfinder and stat. */
	ret = imgu->configureOutput(&imgu->output_, cfg);
	if (ret)
		return ret;

	ret = imgu->configureOutput(&imgu->viewfinder_, cfg);
	if (ret)
		return ret;

	ret = imgu->configureOutput(&imgu->stat_, cfg);
	if (ret)
		return ret;

	return 0;
}

int PipelineHandlerIPU3::allocateBuffers(Camera *camera, Stream *stream)
{
	const StreamConfiguration &cfg = stream->configuration();
	IPU3CameraData *data = cameraData(camera);
	V4L2Device *cio2 = data->cio2_.output_;

	if (!cfg.bufferCount)
		return -EINVAL;

	int ret = cio2->exportBuffers(&stream->bufferPool());
	if (ret) {
		LOG(IPU3, Error) << "Failed to request memory";
		return ret;
	}

	return 0;
}

int PipelineHandlerIPU3::freeBuffers(Camera *camera, Stream *stream)
{
	IPU3CameraData *data = cameraData(camera);
	V4L2Device *cio2 = data->cio2_.output_;

	int ret = cio2->releaseBuffers();
	if (ret) {
		LOG(IPU3, Error) << "Failed to release memory";
		return ret;
	}

	return 0;
}

int PipelineHandlerIPU3::start(Camera *camera)
{
	IPU3CameraData *data = cameraData(camera);
	V4L2Device *cio2 = data->cio2_.output_;
	int ret;

	ret = cio2->streamOn();
	if (ret) {
		LOG(IPU3, Info) << "Failed to start camera " << camera->name();
		return ret;
	}

	return 0;
}

void PipelineHandlerIPU3::stop(Camera *camera)
{
	IPU3CameraData *data = cameraData(camera);
	V4L2Device *cio2 = data->cio2_.output_;

	if (cio2->streamOff())
		LOG(IPU3, Info) << "Failed to stop camera " << camera->name();

	PipelineHandler::stop(camera);
}

int PipelineHandlerIPU3::queueRequest(Camera *camera, Request *request)
{
	IPU3CameraData *data = cameraData(camera);
	V4L2Device *cio2 = data->cio2_.output_;
	Stream *stream = &data->stream_;

	Buffer *buffer = request->findBuffer(stream);
	if (!buffer) {
		LOG(IPU3, Error)
			<< "Attempt to queue request with invalid stream";
		return -ENOENT;
	}

	int ret = cio2->queueBuffer(buffer);
	if (ret < 0)
		return ret;

	PipelineHandler::queueRequest(camera, request);

	return 0;
}

bool PipelineHandlerIPU3::match(DeviceEnumerator *enumerator)
{
	int ret;

	DeviceMatch cio2_dm("ipu3-cio2");
	cio2_dm.add("ipu3-csi2 0");
	cio2_dm.add("ipu3-cio2 0");
	cio2_dm.add("ipu3-csi2 1");
	cio2_dm.add("ipu3-cio2 1");
	cio2_dm.add("ipu3-csi2 2");
	cio2_dm.add("ipu3-cio2 2");
	cio2_dm.add("ipu3-csi2 3");
	cio2_dm.add("ipu3-cio2 3");

	DeviceMatch imgu_dm("ipu3-imgu");
	imgu_dm.add("ipu3-imgu 0");
	imgu_dm.add("ipu3-imgu 0 input");
	imgu_dm.add("ipu3-imgu 0 parameters");
	imgu_dm.add("ipu3-imgu 0 output");
	imgu_dm.add("ipu3-imgu 0 viewfinder");
	imgu_dm.add("ipu3-imgu 0 3a stat");
	imgu_dm.add("ipu3-imgu 1");
	imgu_dm.add("ipu3-imgu 1 input");
	imgu_dm.add("ipu3-imgu 1 parameters");
	imgu_dm.add("ipu3-imgu 1 output");
	imgu_dm.add("ipu3-imgu 1 viewfinder");
	imgu_dm.add("ipu3-imgu 1 3a stat");

	/*
	 * It is safe to acquire both media devices at this point as
	 * DeviceEnumerator::search() skips the busy ones for us.
	 */
	cio2MediaDev_ = enumerator->search(cio2_dm);
	if (!cio2MediaDev_)
		return false;

	cio2MediaDev_->acquire();

	imguMediaDev_ = enumerator->search(imgu_dm);
	if (!imguMediaDev_)
		return false;

	imguMediaDev_->acquire();

	/*
	 * Disable all links that are enabled by default on CIO2, as camera
	 * creation enables all valid links it finds.
	 *
	 * Close the CIO2 media device after, as links are enabled and should
	 * not need to be changed after.
	 */
	if (cio2MediaDev_->open())
		return false;

	if (cio2MediaDev_->disableLinks()) {
		cio2MediaDev_->close();
		return false;
	}

	if (imguMediaDev_->open()) {
		cio2MediaDev_->close();
		return false;
	}

	if (imguMediaDev_->disableLinks())
		goto error;

	ret = registerCameras();

error:
	cio2MediaDev_->close();
	imguMediaDev_->close();

	return ret == 0;
}

/**
 * \brief Initialise ImgU and CIO2 devices associated with cameras
 *
 * Initialise the two ImgU instances and create cameras with an associated
 * CIO2 device instance.
 *
 * \return 0 on success or a negative error code for error or if no camera
 * has been created
 * \retval -ENODEV no camera has been created
 */
int PipelineHandlerIPU3::registerCameras()
{
	int ret;

	ret = imgu0_.init(imguMediaDev_.get(), 0);
	if (ret)
		return ret;

	ret = imgu1_.init(imguMediaDev_.get(), 1);
	if (ret)
		return ret;

	/*
	 * For each CSI-2 receiver on the IPU3, create a Camera if an
	 * image sensor is connected to it and the sensor can produce images
	 * in a compatible format.
	 */
	unsigned int numCameras = 0;
	for (unsigned int id = 0; id < 4 && numCameras < 2; ++id) {
		std::unique_ptr<IPU3CameraData> data =
			utils::make_unique<IPU3CameraData>(this);
		std::set<Stream *> streams{ &data->stream_ };
		CIO2Device *cio2 = &data->cio2_;

		ret = cio2->init(cio2MediaDev_.get(), id);
		if (ret)
			continue;

		/**
		 * \todo Dynamically assign ImgU devices; as of now, limit
		 * support to two cameras only, and assign imgu0 to the first
		 * one and imgu1 to the second.
		 */
		data->imgu_ = numCameras ? &imgu1_ : &imgu0_;

		std::string cameraName = cio2->sensor_->entityName() + " "
				       + std::to_string(id);
		std::shared_ptr<Camera> camera = Camera::create(this,
								cameraName,
								streams);

		cio2->output_->bufferReady.connect(data.get(),
						   &IPU3CameraData::bufferReady);

		registerCamera(std::move(camera), std::move(data));

		LOG(IPU3, Info)
			<< "Registered Camera[" << numCameras << "] \""
			<< cameraName << "\""
			<< " connected to CSI-2 receiver " << id;

		numCameras++;
	}

	return numCameras ? 0 : -ENODEV;
}

void PipelineHandlerIPU3::IPU3CameraData::bufferReady(Buffer *buffer)
{
	Request *request = queuedRequests_.front();

	pipe_->completeBuffer(camera_, request, buffer);
	pipe_->completeRequest(camera_, request);
}

/* -----------------------------------------------------------------------------
 * ImgU Device
 */

/**
 * \brief Initialize components of the ImgU instance
 * \param[in] mediaDevice The ImgU instance media device
 * \param[in] index The ImgU instance index
 *
 * Create and open the V4L2 devices and subdevices of the ImgU instance
 * with \a index.
 *
 * In case of errors the created V4L2Device and V4L2Subdevice instances
 * are destroyed at pipeline handler delete time.
 *
 * \return 0 on success or a negative error code otherwise
 */
int ImgUDevice::init(MediaDevice *media, unsigned int index)
{
	int ret;

	index_ = index;
	name_ = "ipu3-imgu " + std::to_string(index_);
	media_ = media;

	/*
	 * The media entities presence in the media device has been verified
	 * by the match() function: no need to check for newly created
	 * video devices and subdevice validity here.
	 */
	imgu_ = V4L2Subdevice::fromEntityName(media, name_);
	ret = imgu_->open();
	if (ret)
		return ret;

	input_ = V4L2Device::fromEntityName(media, name_ + " input");
	ret = input_->open();
	if (ret)
		return ret;

	output_.dev = V4L2Device::fromEntityName(media, name_ + " output");
	ret = output_.dev->open();
	if (ret)
		return ret;

	output_.pad = PAD_OUTPUT;
	output_.name = "output";

	viewfinder_.dev = V4L2Device::fromEntityName(media,
						     name_ + " viewfinder");
	ret = viewfinder_.dev->open();
	if (ret)
		return ret;

	viewfinder_.pad = PAD_VF;
	viewfinder_.name = "viewfinder";

	stat_.dev = V4L2Device::fromEntityName(media, name_ + " 3a stat");
	ret = stat_.dev->open();
	if (ret)
		return ret;

	stat_.pad = PAD_STAT;
	stat_.name = "stat";

	return 0;
}

/**
 * \brief Configure the ImgU unit input
 * \param[in] config The requested stream configuration
 * \param[in] inputFormat The format to be applied to ImgU input
 *
 * \return 0 on success or a negative error code otherwise
 */
int ImgUDevice::configureInput(const StreamConfiguration &config,
			       V4L2DeviceFormat *inputFormat)
{
	/* Configure the ImgU input video device with the requested sizes. */
	int ret = input_->setFormat(inputFormat);
	if (ret)
		return ret;

	LOG(IPU3, Debug) << "ImgU input format = " << inputFormat->toString();

	/*
	 * \todo The IPU3 driver implementation shall be changed to use the
	 * input sizes as 'ImgU Input' subdevice sizes, and use the desired
	 * GDC output sizes to configure the crop/compose rectangles.
	 *
	 * The current IPU3 driver implementation uses GDC sizes as the
	 * 'ImgU Input' subdevice sizes, and the input video device sizes
	 * to configure the crop/compose rectangles, contradicting the
	 * V4L2 specification.
	 */
	Rectangle rect = {
		.x = 0,
		.y = 0,
		.w = inputFormat->width,
		.h = inputFormat->height,
	};
	ret = imgu_->setCrop(PAD_INPUT, &rect);
	if (ret)
		return ret;

	ret = imgu_->setCompose(PAD_INPUT, &rect);
	if (ret)
		return ret;

	LOG(IPU3, Debug) << "ImgU input feeder and BDS rectangle = "
			 << rect.toString();

	V4L2SubdeviceFormat imguFormat = {};
	imguFormat.width = config.width;
	imguFormat.height = config.height;
	imguFormat.mbus_code = MEDIA_BUS_FMT_FIXED;

	ret = imgu_->setFormat(PAD_INPUT, &imguFormat);
	if (ret)
		return ret;

	LOG(IPU3, Debug) << "ImgU GDC format = " << imguFormat.toString();

	return 0;
}

/**
 * \brief Configure the ImgU unit \a id video output
 * \param[in] output The ImgU output device to configure
 * \param[in] config The requested configuration
 *
 * \return 0 on success or a negative error code otherwise
 */
int ImgUDevice::configureOutput(ImgUOutput *output,
				const StreamConfiguration &config)
{
	V4L2Device *dev = output->dev;
	unsigned int pad = output->pad;

	V4L2SubdeviceFormat imguFormat = {};
	imguFormat.width = config.width;
	imguFormat.height = config.height;
	imguFormat.mbus_code = MEDIA_BUS_FMT_FIXED;

	int ret = imgu_->setFormat(pad, &imguFormat);
	if (ret)
		return ret;

	/* No need to apply format to the stat node. */
	if (output == &stat_)
		return 0;

	V4L2DeviceFormat outputFormat = {};
	outputFormat.width = config.width;
	outputFormat.height = config.height;
	outputFormat.fourcc = V4L2_PIX_FMT_NV12;
	outputFormat.planesCount = 2;

	ret = dev->setFormat(&outputFormat);
	if (ret)
		return ret;

	LOG(IPU3, Debug) << "ImgU " << output->name << " format = "
			 << outputFormat.toString();

	return 0;
}

/*------------------------------------------------------------------------------
 * CIO2 Device
 */

/**
 * \brief Initialize components of the CIO2 device with \a index
 * \param[in] media The CIO2 media device
 * \param[in] index The CIO2 device index
 *
 * Create and open the video device and subdevices in the CIO2 instance at \a
 * index, if a supported image sensor is connected to the CSI-2 receiver of
 * this CIO2 instance.  Enable the media links connecting the CIO2 components
 * to prepare for capture operations and cached the sensor maximum size.
 *
 * \return 0 on success or a negative error code otherwise
 * \retval -ENODEV No supported image sensor is connected to this CIO2 instance
 */
int CIO2Device::init(const MediaDevice *media, unsigned int index)
{
	int ret;

	/*
	 * Verify that a sensor subdevice is connected to this CIO2 instance
	 * and enable the media link between the two.
	 */
	std::string csi2Name = "ipu3-csi2 " + std::to_string(index);
	MediaEntity *csi2Entity = media->getEntityByName(csi2Name);
	const std::vector<MediaPad *> &pads = csi2Entity->pads();
	if (pads.empty())
		return -ENODEV;

	/* IPU3 CSI-2 receivers have a single sink pad at index 0. */
	MediaPad *sink = pads[0];
	const std::vector<MediaLink *> &links = sink->links();
	if (links.empty())
		return -ENODEV;

	MediaLink *link = links[0];
	MediaEntity *sensorEntity = link->source()->entity();
	if (sensorEntity->function() != MEDIA_ENT_F_CAM_SENSOR)
		return -ENODEV;

	ret = link->setEnabled(true);
	if (ret)
		return ret;

	/*
	 * Now that we're sure a sensor subdevice is connected, make sure it
	 * produces at least one image format compatible with CIO2 requirements
	 * and cache the camera maximum size.
	 *
	 * \todo Define when to open and close video device nodes, as they
	 * might impact on power consumption.
	 */
	sensor_ = new V4L2Subdevice(sensorEntity);
	ret = sensor_->open();
	if (ret)
		return ret;

	for (auto it : sensor_->formats(0)) {
		int mbusCode = mediaBusToFormat(it.first);
		if (mbusCode < 0)
			continue;

		for (const SizeRange &size : it.second) {
			if (maxSize_.width < size.maxWidth &&
			    maxSize_.height < size.maxHeight) {
				maxSize_.width = size.maxWidth;
				maxSize_.height = size.maxHeight;
				mbusCode_ = mbusCode;
			}
		}
	}
	if (maxSize_.width == 0) {
		LOG(IPU3, Info) << "Sensor '" << sensor_->entityName()
				<< "' detected, but no supported image format "
				<< " found: skip camera creation";
		return -ENODEV;
	}

	csi2_ = new V4L2Subdevice(csi2Entity);
	ret = csi2_->open();
	if (ret)
		return ret;

	std::string cio2Name = "ipu3-cio2 " + std::to_string(index);
	output_ = V4L2Device::fromEntityName(media, cio2Name);
	ret = output_->open();
	if (ret)
		return ret;

	return 0;
}

/**
 * \brief Configure the CIO2 unit
 * \param[in] config The requested configuration
 * \param[out] outputFormat The CIO2 unit output image format
 *
 * \return 0 on success or a negative error code otherwise
 */
int CIO2Device::configure(const StreamConfiguration &config,
			  V4L2DeviceFormat *outputFormat)
{
	unsigned int imageSize = config.width * config.height;
	V4L2SubdeviceFormat sensorFormat = {};
	unsigned int best = ~0;
	int ret;

	for (auto it : sensor_->formats(0)) {
		/* Only consider formats consumable by the CIO2 unit. */
		if (mediaBusToFormat(it.first) < 0)
			continue;

		for (const SizeRange &size : it.second) {
			/*
			 * Only select formats bigger than the requested sizes
			 * as the IPU3 cannot up-scale.
			 *
			 * \todo: Unconditionally scale on the sensor as much
			 * as possible. This will need to be revisited when
			 * implementing the scaling policy.
			 */
			if (size.maxWidth < config.width ||
			    size.maxHeight < config.height)
				continue;

			unsigned int diff = size.maxWidth * size.maxHeight
					  - imageSize;
			if (diff >= best)
				continue;

			best = diff;

			sensorFormat.width = size.maxWidth;
			sensorFormat.height = size.maxHeight;
			sensorFormat.mbus_code = it.first;
		}
	}

	/*
	 * Apply the selected format to the sensor, the CSI-2 receiver and
	 * the CIO2 output device.
	 */
	ret = sensor_->setFormat(0, &sensorFormat);
	if (ret)
		return ret;

	ret = csi2_->setFormat(0, &sensorFormat);
	if (ret)
		return ret;

	outputFormat->width = sensorFormat.width;
	outputFormat->height = sensorFormat.height;
	outputFormat->fourcc = mediaBusToFormat(sensorFormat.mbus_code);
	outputFormat->planesCount = 1;

	ret = output_->setFormat(outputFormat);
	if (ret)
		return ret;

	LOG(IPU3, Debug) << "CIO2 output format " << outputFormat->toString();

	return 0;
}

int CIO2Device::mediaBusToFormat(unsigned int code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SBGGR10_1X10:
		return V4L2_PIX_FMT_IPU3_SBGGR10;
	case MEDIA_BUS_FMT_SGBRG10_1X10:
		return V4L2_PIX_FMT_IPU3_SGBRG10;
	case MEDIA_BUS_FMT_SGRBG10_1X10:
		return V4L2_PIX_FMT_IPU3_SGRBG10;
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		return V4L2_PIX_FMT_IPU3_SRGGB10;
	default:
		return -EINVAL;
	}
}

REGISTER_PIPELINE_HANDLER(PipelineHandlerIPU3);

} /* namespace libcamera */
