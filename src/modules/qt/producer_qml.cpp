/*
 * producer_qml.cpp -- kdenlive QML producer
 * Copyright (c) 2019 Akhil K Gangadharan <akhilam512@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "qml_wrapper.h"
#include "common.h"
#include <QDebug>
#include <QQmlComponent>
#include <QQmlEngine>

#include <framework/mlt.h>
#include <fstream>
#include <stdexcept>
#include <stdlib.h>
#include <streambuf>
#include <string.h>
#include <string>

void read_qml(mlt_properties properties, mlt_profile profile)
{
      const char *resource = mlt_properties_get(properties, "resource");
      std::ifstream resource_stream;
      resource_stream.open(resource);

      if (!resource_stream)
      {
            throw std::runtime_error(
                "Input QML file was not read - Resource stream error");
      }

      std::string str((std::istreambuf_iterator<char>(resource_stream)),
                      std::istreambuf_iterator<char>());
      const char *infile = str.c_str();
      mlt_properties_set(properties, "_qmldata", infile);
      resource_stream.close();


	QQmlEngine qml_engine;
	QQmlComponent qml_component(&qml_engine, resource, QQmlComponent::PreferSynchronous);

	if (qml_component.isError()) {
		const QList<QQmlError> errorList = qml_component.errors();
		for (const QQmlError &error : errorList)
			qDebug() << "QML Component Error: " << error.url() << error.line() << error;
		return;
	}

      QObject *rootObject = qml_component.create();

      int fps = mlt_profile_fps(profile);
	int duration = getMaxDuration(rootObject);
      int length = duration*fps;
	mlt_properties_set_position(properties, "length", length);
      mlt_properties_set_position(properties, "duration", duration);
      mlt_properties_set_position(properties, "kdenlive:duration", length);
      mlt_properties_set_position(properties, "out", length - 1);
}

static void producer_close(mlt_producer producer)
{
      producer_ktitle_qml self = (producer_ktitle_qml)producer->child;
      producer->close = NULL;
      mlt_service_cache_purge(MLT_PRODUCER_SERVICE(producer));
      mlt_producer_close(producer);
      free(self);
}

static int producer_get_image(mlt_frame frame, uint8_t **buffer,
                              mlt_image_format *format, int *width, int *height,
                              int writable)

{
      int error = 0;
      /* Obtain properties of frame */
      mlt_properties properties = MLT_FRAME_PROPERTIES(frame);

      /* Obtain the producer for this frame */
      producer_ktitle_qml self = (producer_ktitle_qml)mlt_properties_get_data(
          properties, "producer_kdenlivetitle_qml", NULL);

      /* Obtain properties of producer */
      mlt_producer producer = &self->parent;
      mlt_properties producer_props = MLT_PRODUCER_PROPERTIES(producer);
      mlt_profile profile =  mlt_service_profile ( MLT_PRODUCER_SERVICE( producer ) ) ;

      mlt_service_lock(MLT_PRODUCER_SERVICE(producer));

      if ( mlt_properties_get_int( properties, "rescale_width" ) > 0 )
		*width = mlt_properties_get_int( properties, "rescale_width" );
      if ( mlt_properties_get_int( properties, "rescale_height" ) > 0 )
		*height = mlt_properties_get_int( properties, "rescale_height" );

      /* Allocate the image */
      if (mlt_properties_get_int(producer_props, "force_reload"))
      {
            if (mlt_properties_get_int(producer_props, "force_reload") > 1)
                  read_qml(producer_props, profile);
            mlt_properties_set_int(producer_props, "force_reload", 0);
            renderKdenliveTitle(self, frame, *format, *width, *height,
                                mlt_frame_original_position(frame), 1);
      }
      else
      {
            renderKdenliveTitle(self, frame, *format, *width, *height,
                                mlt_frame_original_position(frame), 0);
      }

      *format = self->format;
      if (self->current_image)
      {
            // Clone the image and the alpha
            int image_size = mlt_image_format_size(
                self->format, self->current_width, self->current_height, NULL);
            uint8_t *image_copy = (uint8_t *)mlt_pool_alloc(image_size);
            // We use height-1 because mlt_image_format_size() uses height + 1.
            // XXX Remove -1 when mlt_image_format_size() is changed.
            memcpy(image_copy, self->current_image,
                   mlt_image_format_size(self->format, self->current_width,
                                         self->current_height - 1, NULL));
            // Now update properties so we free the copy after
            mlt_frame_set_image(frame, image_copy, image_size,
                                mlt_pool_release);
            // We're going to pass the copy on
            *buffer = image_copy;

            // Clone the alpha channel
            if (self->current_alpha)
            {
                  image_copy = (uint8_t *)mlt_pool_alloc(self->current_width *
                                                         self->current_height);
                  memcpy(image_copy, self->current_alpha,
                         self->current_width * self->current_height);
                  mlt_frame_set_alpha(frame, image_copy,
                                      self->current_width *
                                          self->current_height,
                                      mlt_pool_release);
            }
      }
      else
      {
            error = 1;
      }

      mlt_service_unlock(MLT_PRODUCER_SERVICE(producer));

      return error;
}

static int producer_get_frame(mlt_producer producer, mlt_frame_ptr frame,
                              int index)
{
      producer_ktitle_qml self = (producer_ktitle_qml)producer->child;
      *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));

      if (*frame != NULL)
      {
            /* Obtain properties of frame and producer */
            mlt_properties properties = MLT_FRAME_PROPERTIES(*frame);

            /* Obtain properties of producer */
            mlt_properties producer_props = MLT_PRODUCER_PROPERTIES(producer);

            /* Set the producer on the frame properties */
            mlt_properties_set_data(properties, "producer_kdenlivetitle_qml",
                                    self, 0, NULL, NULL);

            /* Update timecode on the frame we're creating */
            mlt_frame_set_position(*frame, mlt_producer_position(producer));

            /* Push the get_image method */
            mlt_frame_push_get_image(*frame, producer_get_image);
      }

      /* Calculate the next timecode */
      mlt_producer_prepare_next(producer);

      return 0;
}

extern "C"
{
      mlt_producer producer_qml_init(mlt_profile profile, mlt_service_type type,
                                     const char *id, char *filename)
      {
	    /* Create a new producer object */
            producer_ktitle_qml self = (producer_ktitle_qml)calloc(
                1, sizeof(struct producer_ktitle_qml_s));

            if (self != NULL && mlt_producer_init(&self->parent, self) == 0)
            {
                  mlt_producer producer = &self->parent;

                  /* Get the properties interface */
                  if (!createQApplicationIfNeeded(
                          MLT_PRODUCER_SERVICE(producer)))
                  {
                        mlt_producer_close(producer);
                        return NULL;
                  }

                  mlt_properties properties = MLT_PRODUCER_PROPERTIES(producer);
                  mlt_properties_set(properties, "resource", filename);

                  read_qml(properties, profile);
                  /* Callback registration */
                  producer->get_frame = producer_get_frame;
                  producer->close = (mlt_destructor)producer_close;
                  mlt_properties_set_int(properties, "progressive", 1);
                  mlt_properties_set_int(properties, "aspect_ratio", 1);
                  mlt_properties_set_int(properties, "seekable", 1);
                  return producer;
            }
	    free(self);
            return NULL;
      }
}
