/*
 * qml_wrapper.cpp -- QML wrapper
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
#include <framework/mlt_log.h>
#include <QString>
#include <QSize>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFramebufferObject>
#include <QQuickRenderControl>
#include <QOffscreenSurface>
#include <QEvent>
#include <QtConcurrent/QtConcurrent>
#include <QQmlError>
#include <QFuture>
#include <QQuickWindow>
#include <QThread>
#include <QEventLoop>
#include <QMutex>
#include <QWaitCondition>
#include <QtCore/QAnimationDriver>

static const QEvent::Type INIT = QEvent::Type(QEvent::User + 1);
static const QEvent::Type RENDER = QEvent::Type(QEvent::User + 2);
static const QEvent::Type RESIZE = QEvent::Type(QEvent::User + 3);
static const QEvent::Type STOP = QEvent::Type(QEvent::User + 4);
static const QEvent::Type UPDATE = QEvent::Type(QEvent::User + 5);
class QmlAnimationDriver : public QAnimationDriver
{
private:
    int m_step;
    qint64 m_elapsed;

public:
    QmlAnimationDriver(int msPerStep): m_step(msPerStep), m_elapsed(0) {};

    void advance() override
	{
		m_elapsed += m_step;
		advanceAnimation();
	}

    qint64 elapsed() const override
	{
		return m_elapsed;
	}


};


class QmlCoreRenderer : public QObject
{
private:

    QWaitCondition m_cond;
    QMutex m_mutex;
    QOpenGLContext* m_context;
    QOffscreenSurface* m_offscreenSurface;
    QQuickRenderControl* m_renderControl;
    QQuickWindow* m_quickWindow;
    QOpenGLFramebufferObject* m_fbo;
    QmlAnimationDriver* m_animationDriver;
    QImage::Format m_format;
    QSize m_size;
    qreal m_dpr;
    QMutex m_quitMutex;
    int m_fps;
    QImage m_image;

public:
    explicit QmlCoreRenderer(QObject *parent = nullptr):QObject(parent),
														m_fbo(nullptr),
														m_animationDriver(nullptr),
														m_offscreenSurface(nullptr),
														m_context(nullptr),
														m_quickWindow(nullptr),
														m_renderControl(nullptr) {}
    ~QmlCoreRenderer() override {}

    void requestInit() { QCoreApplication::postEvent(this, new QEvent(INIT)); }
    void requestRender() { QCoreApplication::postEvent(this, new QEvent(RENDER)); }
    void requestResize() { QCoreApplication::postEvent(this, new QEvent(RESIZE)); }
    void requestStop() { QCoreApplication::postEvent(this, new QEvent(STOP)); }
    void setContext(QOpenGLContext *context) { m_context = context; }
    void setSurface(QOffscreenSurface *surface) { m_offscreenSurface = surface; }
    void setQuickWindow(QQuickWindow *window) { m_quickWindow = window; }
    void setRenderControl(QQuickRenderControl* control) { m_renderControl = control; }
    void setAnimationDriver(QmlAnimationDriver* driver) { m_animationDriver = driver; }
    void setSize(QSize s) { m_size = s; }
    void setDPR(qreal value) { m_dpr = value; }
    void setFPS(int value) { m_fps = value;}
    void setFormat( QImage::Format f) { m_format = f; }
    void checkifAnimDriverRunning() { m_animationDriver->isRunning()? qDebug() << " 1 driver running": qDebug() << "2  driver is NOT running :)"; }
    QWaitCondition *cond() { return &m_cond; }
    QMutex *mutex() { return &m_mutex; }
    QImage getRenderedQImage() { return m_image; }

private:
    void cleanup()
	{
		m_context->makeCurrent(m_offscreenSurface);
		m_renderControl->invalidate();
		delete m_fbo;
		m_fbo = nullptr;
		m_context->doneCurrent();

		m_context->moveToThread(QCoreApplication::instance()->thread());
		m_cond.wakeOne();
	}

    void init()
	{
		m_context->makeCurrent(m_offscreenSurface);
		m_renderControl->initialize(m_context);
	}

    void ensureFbo()
	{
		Q_ASSERT(!m_size.isEmpty());
		Q_ASSERT(m_dpr != 0.0);

		if (m_fbo && m_fbo->size() != m_size * m_dpr) {
			delete m_fbo;
			m_fbo = nullptr;
		}

		if (!m_fbo) {
			m_fbo = new QOpenGLFramebufferObject(m_size * m_dpr, QOpenGLFramebufferObject::CombinedDepthStencil);
			m_quickWindow->setRenderTarget(m_fbo);
			Q_ASSERT(m_quickWindow->isSceneGraphInitialized());
		}
	}

    void render(QMutexLocker *lock)
	{
		if (!m_context->makeCurrent(m_offscreenSurface)) {
        qWarning("!!!!! ERROR : Failed to make context current on render thread");
        return;
    	}

		ensureFbo();
		m_renderControl->sync();

		// NOTE: normally, in a gui application, the main thread would not be blocked whilst rendering takes place on the render thread.
		// The main thread, in our case,  must wait untill the rendering is done because we only care about the final rendered result

		m_renderControl->render();
		m_context->functions()->glFlush();

		m_image = m_fbo->toImage();
		m_image.convertTo(m_format);

		m_cond.wakeOne();
		lock->unlock();
	}

    bool event(QEvent *e) override
	{
		QMutexLocker lock(&m_mutex);
		switch (int(e->type())) {
			case INIT:
				init();
				return true;
			case RENDER:
				render(&lock);
				return true;
			case RESIZE:
				// TODO
				return true;
			case STOP:
				cleanup();
				return true;
			default:
				return QObject::event(e);
		}
	}
};


class QmlRenderer: public QObject
{
    Q_OBJECT

    QOpenGLContext *m_context;
    QOffscreenSurface *m_offscreenSurface;
    QQuickRenderControl* m_renderControl;
    QQuickWindow *m_quickWindow;
    QQmlEngine *m_qmlEngine;
    QQmlComponent *m_qmlComponent;
    QQuickItem *m_rootItem;
    QOpenGLFramebufferObject *m_fbo;
    QmlAnimationDriver *m_animationDriver;
    QmlCoreRenderer *m_corerenderer;
    QThread *m_rendererThread;
    enum renderStatus {
        NotRunning,
        Initialised
    };

    qreal m_dpr;
    QSize m_size;
    renderStatus m_status;
    int m_duration;
    int m_fps;
    int m_framesCount;
    mlt_position m_currentFrame;
    QUrl m_qmlFileUrl;
    QImage m_frame;
    mlt_position m_requestedFrame;
    QImage::Format m_ImageFormat;
    QImage m_img;
    mlt_position m_totalFrames;
    QWaitCondition m_cond;
    QMutex m_mutex;

signals:
    void imageReady();

public:
    explicit QmlRenderer(QString qmlFileUrlString, int fps, int duration, QObject *parent = nullptr)
		    : QObject(parent)
    , m_fbo(nullptr)
    , m_animationDriver(nullptr)
    , m_offscreenSurface(nullptr)
    , m_context(nullptr)
    , m_quickWindow(nullptr)
    , m_renderControl(nullptr)
    , m_rootItem(nullptr)
    , m_corerenderer(nullptr)
    , m_qmlComponent(nullptr)
    , m_qmlEngine(nullptr)
    , m_status(NotRunning)
    , m_qmlFileUrl(qmlFileUrlString)
    , m_dpr(1.0)
    , m_duration(duration)
    , m_fps(fps)
    , m_currentFrame(0)
    , m_framesCount(fps*duration)
	{
		QSurfaceFormat format;
		format.setDepthBufferSize(16);
		format.setStencilBufferSize(8);
		m_context = new QOpenGLContext();
		m_context->setFormat(format);
		Q_ASSERT(format.depthBufferSize() == (m_context->format()).depthBufferSize());
		Q_ASSERT(format.stencilBufferSize() == (m_context->format()).stencilBufferSize());
		m_context->create();
		Q_ASSERT(m_context->isValid());

		m_offscreenSurface = new QOffscreenSurface();
		m_offscreenSurface->setFormat(m_context->format());
		m_offscreenSurface->create();
		Q_ASSERT(m_offscreenSurface->isValid());

		m_renderControl = new QQuickRenderControl(this);
		Q_ASSERT(m_renderControl != nullptr);
		QQmlEngine::setObjectOwnership(m_renderControl, QQmlEngine::CppOwnership);

		m_quickWindow = new QQuickWindow(m_renderControl);
		Q_ASSERT(m_quickWindow != nullptr);

		m_qmlEngine = new QQmlEngine();
		if (!m_qmlEngine->incubationController()) {
			m_qmlEngine->setIncubationController(m_quickWindow->incubationController());
		}

		m_corerenderer = new QmlCoreRenderer();
		m_corerenderer->setContext(m_context);
		m_corerenderer->setSurface(m_offscreenSurface);
		m_corerenderer->setQuickWindow(m_quickWindow);
		m_corerenderer->setRenderControl(m_renderControl);
		m_corerenderer->setDPR(m_dpr);
		m_corerenderer->setFPS(m_fps);

		m_rendererThread = new QThread;
		m_renderControl->prepareThread(m_rendererThread);

		m_context->moveToThread(m_rendererThread);
		m_corerenderer->moveToThread(m_rendererThread);
		m_rendererThread->start();

		connect(
			m_quickWindow, &QQuickWindow::sceneGraphError,
			[=]( QQuickWindow::SceneGraphError error, const QString &message) {
				qDebug() << "!!!!!!!! ERROR - QML Scene Graph: " << error << message;
				}
		);
		connect(
			m_qmlEngine, &QQmlEngine::warnings,
			[=]( QList<QQmlError> warnings) {
				foreach(const QQmlError& warning, warnings) {
					qDebug() << "!!!! QML WARNING : "  << warning << "  " ;
				}
			}
		);
	}

    ~QmlRenderer() override
	{
		m_corerenderer->mutex()->lock();
		m_corerenderer->requestStop();
		m_corerenderer->cond()->wait(m_corerenderer->mutex());
		m_corerenderer->mutex()->unlock();

		m_rendererThread->quit();
		m_rendererThread->wait();

		m_context->makeCurrent(m_offscreenSurface);
		m_renderControl->invalidate();
		delete m_fbo;
		m_fbo = nullptr;

		m_context->doneCurrent();

		delete m_context;
		delete m_offscreenSurface;

	}

    QImage render(int width, int height, QImage::Format format)
	{
		init(width, height, format);
		return m_img;
	}

    QImage render(int width, int height, QImage::Format format, int frame)
	{
		m_requestedFrame = frame;
		m_currentFrame = 0;
		init(width, height, format);
		installEventFilter(this);

		QEventLoop loop;
		connect(this, &QmlRenderer::imageReady, &loop, &QEventLoop::quit, Qt::QueuedConnection);
		renderAnimated();
		loop.exec();
		resetDriver();
		delete m_qmlComponent;
		delete m_rootItem;

		return m_img;
	}

    void checkCurrentContex() {    m_context->currentContext() == nullptr? qDebug() << "1 Context is Null ": qDebug() << "2 A context was made current!"; }
    void checkifAnimDriverRunning() { m_animationDriver->isRunning()? qDebug() << " 1 driver running": qDebug() << "2  driver is NOT running :)"; }

protected:
    bool eventFilter(QObject *obj, QEvent *event) override
	{
		if(event->type() == QEvent::UpdateRequest)
		{
			renderAnimated();
			return true;
		}
		return QObject::event(event);
	}

private:
    void initDriver()
	{
		m_animationDriver = new QmlAnimationDriver(1000 / m_fps);
		m_animationDriver->install();
		m_corerenderer->setAnimationDriver(m_animationDriver);
	}

    void resetDriver()
	{
		m_animationDriver->uninstall();
		delete m_animationDriver;
		m_animationDriver = nullptr;
	}

    void init(int width, int height, QImage::Format imageFormat)
	{
		if (m_status == NotRunning || m_duration > 0) {
			initDriver();
			m_size = QSize(width, height);
			m_ImageFormat = imageFormat;
			m_corerenderer->setSize(m_size);
			m_corerenderer->setFormat(m_ImageFormat);
			loadInput();
			m_corerenderer->requestInit();
			m_status = Initialised;
		}
	}

    void loadInput()
	{
		m_qmlComponent = new QQmlComponent(m_qmlEngine, QUrl(m_qmlFileUrl), QQmlComponent::PreferSynchronous);
		Q_ASSERT(!m_qmlComponent->isNull() || m_qmlComponent->isReady());
		bool assert = loadRootObject();
		Q_ASSERT(assert);
		Q_ASSERT(!m_size.isEmpty());
		m_rootItem->setWidth(m_size.width());
		m_rootItem->setHeight(m_size.height());
		m_quickWindow->setGeometry(0, 0, m_size.width(), m_size.height());
	}

    void polishSyncRender()
	{
		// Polishing happens on the main thread
		m_renderControl->polishItems();
		// Sync and render happens on the render thread with the main thread (this one) blocked
		QMutexLocker lock(m_corerenderer->mutex());
		m_corerenderer->requestRender();
		// Wait until sync and render is complete
		m_corerenderer->cond()->wait(m_corerenderer->mutex());
	}

    bool loadRootObject()
	{
	    if(!checkQmlComponent()) {
        return false;
		}
		Q_ASSERT(m_qmlComponent->create() != nullptr);
		QObject *rootObject = m_qmlComponent->create();
		QQmlEngine::setObjectOwnership(rootObject, QQmlEngine::CppOwnership);
		Q_ASSERT(rootObject);
		if(!checkQmlComponent()) {
			return false;
		}
		m_rootItem = qobject_cast<QQuickItem*>(rootObject);
		if (!m_rootItem) {
			qDebug()<< "ERROR - run: Not a QQuickItem - QML file INVALID ";
			delete rootObject;
			return false;
		}
		m_rootItem->setParentItem(m_quickWindow->contentItem());
		return true;
	}

    bool checkQmlComponent()
	{
		if (m_qmlComponent->isError()) {
			const QList<QQmlError> errorList = m_qmlComponent->errors();
			for (const QQmlError &error : errorList) {
				qDebug() <<"QML Component Error: " << error.url() << error.line() << error;
			}
			return false;
		}
		return true;
	}

    void renderStatic()
	{
		polishSyncRender();
		m_img = m_corerenderer->getRenderedQImage();
	}

	void renderAnimated()
	{
		polishSyncRender();
		m_animationDriver->advance();

		if(m_currentFrame == m_requestedFrame) {
			m_img =  m_corerenderer->getRenderedQImage();
			emit imageReady();
		}

		m_currentFrame++;

		if (m_currentFrame < m_framesCount) {
			QEvent *updateRequest = new QEvent(QEvent::UpdateRequest);
			QCoreApplication::postEvent(this, updateRequest);
		}

		else {
			removeEventFilter(this);
			emit imageReady();
			return;
		}
	}

};

static void qrenderer_delete(void *data)
{
	qDebug() << " HERE IN QRENDERE DELETE " ;
       QmlRenderer *renderer = (QmlRenderer*) data;
       if (renderer)
               delete renderer;

       renderer = NULL;
}


int getIntProp(QObject* object, QString propertyName)
{
    for(int i=0; i<object->metaObject()->propertyCount(); i++) {
        QMetaProperty prop = object->metaObject()->property(i);
        if(prop.name() == propertyName) {
            int propertyValue = prop.read(object).toInt();
            if(propertyValue > 0) {
				if(propertyName == QString("duration"))
					propertyValue = propertyValue/1000; //convert from milliseconds to seconds

                return propertyValue;
            }
        }
    }
    return 0;
}

int getMaxDuration(QObject *root, int duration)
{
    if (root == nullptr)
        return 0;

    QString name = root->metaObject()->className();
    if(name.contains("Sequential")) {
        std::vector<int> durationList;
        int dur_seq = 0;
        foreach(QObject* child, root->children()) {
            int childDuration = getMaxDuration(child);
			dur_seq += childDuration;
        }
        return dur_seq;

    }
    else if(name.contains("Parallel")) {
        std::vector<int> durationList;
        int dur_par = 0;
        for(auto &child: root->children()) {
            durationList.push_back(getMaxDuration(child));
        }
        return *max_element(durationList.begin(),durationList.end());
    }
    else if(name.contains("NumberAnimation") || name.contains("ColorAnimation") || name.contains("PauseAnimation") || name.contains("PathAnimation") || name.contains("RotationAnimation")) {
        int dur = getIntProp(root, "duration");
        return dur;
    }
    else {
        foreach(QObject* child, root->children()) {
            int duration = getMaxDuration(child);
			if( duration > 0) {
				return duration;
			}
		}
	}
}

void loadFromQml(producer_ktitle_qml self)
{

	// TODO: Read length values from qml file, if present. Tip: Use QtQobject to store the values in qml
	mlt_producer producer = &self->parent;
	mlt_properties producer_props = MLT_PRODUCER_PROPERTIES( producer );
	mlt_profile profile = mlt_service_profile ( MLT_PRODUCER_SERVICE( producer ) ) ;

	// QQmlEngine qml_engine;
	// QQmlComponent qml_component(&qml_engine, mlt_properties_get( producer_props, "resource"), QQmlComponent::PreferSynchronous);

	// if (qml_component.isError()) {
	// 	const QList<QQmlError> errorList = qml_component.errors();
	// 	for (const QQmlError &error : errorList)
	// 		qDebug() << "QML Component Error: " << error.url() << error.line() << error;
	// 	return;
	// }

    // QObject *rootObject = qml_component.create();
}

void renderKdenliveTitle(producer_ktitle_qml self, mlt_frame frame,
                         mlt_image_format format, int width, int height,
                         mlt_position position, int force_refresh)
{
	// Obtain the producer
	mlt_producer producer = &self->parent;
	mlt_profile profile = mlt_service_profile ( MLT_PRODUCER_SERVICE( producer ) ) ;
	mlt_properties producer_props = MLT_PRODUCER_PROPERTIES(producer);
	mlt_properties properties = MLT_FRAME_PROPERTIES(frame);
	pthread_mutex_lock(&self->mutex);

	int anim_duration = mlt_properties_get_int(producer_props, "duration");
	bool animated = anim_duration > 0? true:false;
	double fps =  mlt_profile_fps(profile);
	if ( mlt_properties_get( producer_props, "duration" ) != NULL || force_refresh == 1 || width != self->current_width || height != self->current_height || animated) {

		if( !animated ) {
			self->current_image = NULL;
			mlt_properties_set_data( producer_props, "_cached_image", NULL, 0, NULL, NULL );
		}
		mlt_properties_set_int( producer_props, "force_reload", 0 );
	}


	int image_size = width * height * 4;
	if ( self->current_image == NULL || animated) {
		QmlRenderer *renderer = static_cast<QmlRenderer *> (mlt_properties_get_data( producer_props, "qrenderer", NULL));
		self->current_alpha = NULL;

		if ( force_refresh == 1 && renderer ) {
			renderer = NULL;
			mlt_properties_set_data( producer_props, "qrenderer", NULL, 0, NULL, NULL);
		}

		if ( renderer == NULL ) {
			if (!createQApplicationIfNeeded(MLT_PRODUCER_SERVICE(producer))) {
				pthread_mutex_unlock(&self->mutex);
				return;
			}
			renderer = new QmlRenderer(mlt_properties_get( producer_props, "resource"), fps, anim_duration);

			mlt_properties_set_data( producer_props, "qrenderer", renderer, 0, ( mlt_destructor )qrenderer_delete, NULL );

		}
		self->rgba_image = (uint8_t *)mlt_pool_alloc(image_size);
#if QT_VERSION >= 0x050200
		// QImage::Format_RGBA8888 was added in Qt5.2
		// Initialize the QImage with the MLT image because the data formats
		// match.
		QImage img(self->rgba_image, width, height, QImage::Format_RGBA8888);
#else
		QImage img(width, height, QImage::Format_ARGB32);
#endif
		img.fill(0);
		QImage rendered_img;
		 if (animated) {
			mlt_position anim_out = mlt_properties_get_position( producer_props, "out" );
			mlt_position length = mlt_properties_get_position(producer_props, "length");

			if( position > anim_out) {
				rendered_img = renderer->render(width, height, img.format(), length+1);
			}
			else {
				rendered_img = renderer->render(width, height, img.format(), position);
			}
		 }
		else {
			rendered_img = renderer->render(width, height, img.format());
		}
		memcpy(img.scanLine(0), rendered_img.constBits(), img.width() * img.height()*4);
		self->format = mlt_image_rgb24a;
		convert_qimage_to_mlt_rgba(&img, self->rgba_image, width, height);
		self->current_image = (uint8_t *)mlt_pool_alloc(image_size);
		memcpy(self->current_image, self->rgba_image, image_size);

		mlt_properties_set_data(producer_props, "_cached_buffer", self->rgba_image, image_size, mlt_pool_release, NULL);
		mlt_properties_set_data(producer_props, "_cached_image", self->current_image, image_size, mlt_pool_release, NULL);
		self->current_width = width;
		self->current_height = height;
		uint8_t *alpha = NULL;

		if ((alpha = mlt_frame_get_alpha(frame))) {
			self->current_alpha = (uint8_t *)mlt_pool_alloc(width * height);
			memcpy(self->current_alpha, alpha, width * height);
			mlt_properties_set_data(producer_props, "_cached_alpha", self->current_alpha, width * height, mlt_pool_release, NULL);
		}
	}
	// Convert image to requested format
	if (format != mlt_image_none && format != mlt_image_glsl && format != self->format) {
		uint8_t *buffer = NULL;
		if (self->format != mlt_image_rgb24a) {
				// Image buffer was previously converted, revert to original
				// rgba buffer
				self->current_image = (uint8_t *)mlt_pool_alloc(image_size);
				memcpy(self->current_image, self->rgba_image, image_size);
				mlt_properties_set_data(producer_props, "_cached_image", self->current_image, image_size, mlt_pool_release, NULL);
				self->format = mlt_image_rgb24a;
		}

		// First, set the image so it can be converted when we get it
		mlt_frame_replace_image(frame, self->current_image, self->format, width, height);
		mlt_frame_set_image(frame, self->current_image, image_size, NULL);
		self->format = format;

		// get_image will do the format conversion
		mlt_frame_get_image(frame, &buffer, &format, &width, &height, 0);

		// cache copies of the image and alpha buffers
		if (buffer) {
				image_size = mlt_image_format_size(format, width, height, NULL);
				self->current_image = (uint8_t *)mlt_pool_alloc(image_size);
				memcpy(self->current_image, buffer, image_size);
				mlt_properties_set_data(producer_props, "_cached_image", self->current_image, image_size, mlt_pool_release, NULL);
		}
		if ((buffer = mlt_frame_get_alpha(frame))) {
				self->current_alpha = (uint8_t *)mlt_pool_alloc(width * height);
				memcpy(self->current_alpha, buffer, width * height);
				mlt_properties_set_data(producer_props, "_cached_alpha", self->current_alpha, width * height, mlt_pool_release, NULL);
		}
	}
	pthread_mutex_unlock(&self->mutex);
	mlt_properties_set_int(properties, "width", self->current_width);
	mlt_properties_set_int(properties, "height", self->current_height);
}
#include "qml_wrapper.moc"
