/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * v4l2_camera.h - V4L2 compatibility camera
 */

#ifndef __V4L2_CAMERA_H__
#define __V4L2_CAMERA_H__

#include <deque>
#include <linux/videodev2.h>
#include <mutex>

#include <libcamera/buffer.h>
#include <libcamera/camera.h>

#include "semaphore.h"

using namespace libcamera;

class FrameMetadata
{
public:
	FrameMetadata(Buffer *buffer);

	int index() const { return index_; }

	unsigned int bytesused() const { return bytesused_; }
	uint64_t timestamp() const { return timestamp_; }
	unsigned int sequence() const { return sequence_; }

	Buffer::Status status() const { return status_; }

private:
	int index_;

	unsigned int bytesused_;
	uint64_t timestamp_;
	unsigned int sequence_;

	Buffer::Status status_;
};

class V4L2Camera : public Object
{
public:
	V4L2Camera(std::shared_ptr<Camera> camera);
	~V4L2Camera();

	int open();
	void close();
	void getStreamConfig(StreamConfiguration *streamConfig);
	std::vector<FrameMetadata> completedBuffers();

	void *mmap(unsigned int index);

	int configure(StreamConfiguration *streamConfigOut,
		      const Size &size, PixelFormat pixelformat,
		      unsigned int bufferCount);

	int allocBuffers(unsigned int count);
	void freeBuffers();
	int streamOn();
	int streamOff();

	int qbuf(unsigned int index);

	Semaphore bufferSema_;

private:
	void requestComplete(Request *request);

	std::shared_ptr<Camera> camera_;
	std::unique_ptr<CameraConfiguration> config_;

	bool isRunning_;

	std::mutex bufferLock_;

	std::deque<std::unique_ptr<Request>> pendingRequests_;
	std::deque<std::unique_ptr<FrameMetadata>> completedBuffers_;
};

#endif /* __V4L2_CAMERA_H__ */
