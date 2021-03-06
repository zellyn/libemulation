
/**
 * libemulation-hal
 * OpenGL canvas
 * (C) 2010-2011 by Marc S. Ressl (mressl@umich.edu)
 * Released under the GPL
 *
 * Implements an OpenGL canvas
 */

#include <math.h>
#include <sys/time.h>

#include "OpenGLCanvas.h"

#include "OEVector.h"
#include "OEMatrix3.h"

// References:
// * Poynton C., Digital Video and HDTV Algorithms and Interfaces

#define NTSC_I_CUTOFF               1300000
#define NTSC_Q_CUTOFF               600000
#define NTSC_IQ_DELTA               (NTSC_I_CUTOFF - NTSC_Q_CUTOFF)

#define PAPER_SLICE                 256

#define BEZELCAPTURE_DISPLAY_TIME   2.0
#define BEZELCAPTURE_FADEOUT_TIME   0.5

static const char *rgbShader = "\
uniform sampler2D texture;\n\
uniform vec2 textureSize;\n\
uniform vec3 c0, c1, c2, c3, c4, c5, c6, c7, c8;\n\
uniform mat3 decoderMatrix;\n\
uniform vec3 decoderOffset;\n\
\n\
vec3 pixel(vec2 q)\n\
{\n\
return texture2D(texture, q).rgb;\n\
}\n\
\n\
vec3 pixels(in vec2 q, in float i)\n\
{\n\
return pixel(vec2(q.x + i, q.y)) + pixel(vec2(q.x - i, q.y));\n\
}\n\
\n\
void main(void)\n\
{\n\
vec2 q = gl_TexCoord[0].xy;\n\
vec3 c = pixel(q) * c0;\n\
c += pixels(q, 1.0 / textureSize.x) * c1;\n\
c += pixels(q, 2.0 / textureSize.x) * c2;\n\
c += pixels(q, 3.0 / textureSize.x) * c3;\n\
c += pixels(q, 4.0 / textureSize.x) * c4;\n\
c += pixels(q, 5.0 / textureSize.x) * c5;\n\
c += pixels(q, 6.0 / textureSize.x) * c6;\n\
c += pixels(q, 7.0 / textureSize.x) * c7;\n\
c += pixels(q, 8.0 / textureSize.x) * c8;\n\
gl_FragColor = vec4(decoderMatrix * c + decoderOffset, 1.0);\n\
}";

static const char *compositeShader = "\
uniform sampler2D texture;\n\
uniform vec2 textureSize;\n\
uniform float subcarrier;\n\
uniform sampler1D phaseInfo;\n\
uniform vec3 c0, c1, c2, c3, c4, c5, c6, c7, c8;\n\
uniform mat3 decoderMatrix;\n\
uniform vec3 decoderOffset;\n\
\n\
float PI = 3.14159265358979323846264;\n\
\n\
vec3 pixel(in vec2 q)\n\
{\n\
vec3 c = texture2D(texture, q).rgb;\n\
vec2 p = texture1D(phaseInfo, q.y).rg;\n\
float phase = 2.0 * PI * (subcarrier * textureSize.x * q.x + p.x);\n\
return c * vec3(1.0, sin(phase), (1.0 - 2.0 * p.y) * cos(phase));\n\
}\n\
\n\
vec3 pixels(vec2 q, float i)\n\
{\n\
return pixel(vec2(q.x + i, q.y)) + pixel(vec2(q.x - i, q.y));\n\
}\n\
\n\
void main(void)\n\
{\n\
vec2 q = gl_TexCoord[0].st;\n\
vec3 c = pixel(q) * c0;\n\
c += pixels(q, 1.0 / textureSize.x) * c1;\n\
c += pixels(q, 2.0 / textureSize.x) * c2;\n\
c += pixels(q, 3.0 / textureSize.x) * c3;\n\
c += pixels(q, 4.0 / textureSize.x) * c4;\n\
c += pixels(q, 5.0 / textureSize.x) * c5;\n\
c += pixels(q, 6.0 / textureSize.x) * c6;\n\
c += pixels(q, 7.0 / textureSize.x) * c7;\n\
c += pixels(q, 8.0 / textureSize.x) * c8;\n\
gl_FragColor = vec4(decoderMatrix * c + decoderOffset, 1.0);\n\
}";

static const char *displayShader = "\
uniform sampler2D texture;\n\
uniform vec2 textureSize;\n\
uniform float barrel;\n\
uniform vec2 barrelSize;\n\
uniform float scanlineLevel;\n\
uniform sampler2D shadowMask;\n\
uniform vec2 shadowMaskSize;\n\
uniform float shadowMaskLevel;\n\
uniform float centerLighting;\n\
uniform sampler2D persistence;\n\
uniform vec2 persistenceSize;\n\
uniform vec2 persistenceOrigin;\n\
uniform float persistenceLevel;\n\
uniform float luminanceGain;\n\
\n\
float PI = 3.14159265358979323846264;\n\
\n\
void main(void)\n\
{\n\
vec2 qc = (gl_TexCoord[1].st - vec2(0.5, 0.5)) * barrelSize;\n\
vec2 qb = barrel * qc * dot(qc, qc);\n\
vec2 q = gl_TexCoord[0].st + qb;\n\
\n\
vec3 c = texture2D(texture, q).rgb;\n\
\n\
float scanline = sin(PI * textureSize.y * q.y);\n\
c *= mix(1.0, scanline * scanline, scanlineLevel);\n\
\n\
vec3 mask = texture2D(shadowMask, (gl_TexCoord[1].st + qb) * shadowMaskSize).rgb;\n\
c *= mix(vec3(1.0, 1.0, 1.0), mask, shadowMaskLevel);\n\
\n\
vec2 lighting = qc * centerLighting;\n\
c *= exp(-dot(lighting, lighting));\n\
\n\
c *= luminanceGain;\n\
\n\
vec2 qp = gl_TexCoord[1].st * persistenceSize + persistenceOrigin;\n\
c = max(c, texture2D(persistence, qp).rgb * persistenceLevel - 0.5 / 256.0);\n\
\n\
gl_FragColor = vec4(c, 1.0);\n\
}";

OpenGLCanvas::OpenGLCanvas(string resourcePath, OECanvasType canvasType)
{
    this->resourcePath = resourcePath;
    this->canvasType = canvasType;
    
    setCapture = NULL;
    setKeyboardLEDs = NULL;
    userData = NULL;
    
    isOpen = false;
    isShaderEnabled = true;
    
    pthread_mutex_init(&mutex, NULL);
    
    captureMode = CANVAS_NO_CAPTURE;
    
    viewportSize = OEMakeSize(640, 480);
    isViewportUpdated = true;
    
    isImageUpdated = false;
    imageSampleRate = 0;
    imageBlackLevel = 0;
    imageWhiteLevel = 0;
    
    printPosition = OEMakePoint(0, 0);
    
    bezel = CANVAS_BEZEL_NONE;
    isBezelDrawRequired = false;
    isBezelCapture = false;
    
    persistenceTexRect = OEMakeRect(0, 0, 0, 0);
}

OpenGLCanvas::~OpenGLCanvas()
{
    pthread_mutex_destroy(&mutex);
}

void OpenGLCanvas::lock()
{
    pthread_mutex_lock(&mutex);
}

void OpenGLCanvas::unlock()
{
    pthread_mutex_unlock(&mutex);
}

// Video

void OpenGLCanvas::open(CanvasSetCapture setCapture,
                        CanvasSetKeyboardLEDs setKeyboardLEDs,
                        void *userData)
{
    this->setCapture = setCapture;
    this->setKeyboardLEDs = setKeyboardLEDs;
    this->userData = userData;
    
    initOpenGL();
}

void OpenGLCanvas::close()
{
    freeOpenGL();
}

void OpenGLCanvas::setEnableShader(bool value)
{
    lock();
    
    isShaderEnabled = value;
    isConfigurationUpdated = true;
    
    unlock();
}

OECanvasType OpenGLCanvas::getCanvasType()
{
    return canvasType;
}

OESize OpenGLCanvas::getDefaultViewportSize()
{
    OESize size;
    
    lock();
    
    if (canvasType == OECANVAS_DISPLAY)
        size = displayConfiguration.displayResolution;
    else if (canvasType == OECANVAS_PAPER)
        size = OEMakeSize(512, 384);
    else if (canvasType == OECANVAS_OPENGL)
        size = openGLConfiguration.viewportDefaultSize;
    
    unlock();
    
    return size;
}

void OpenGLCanvas::setViewportSize(OESize size)
{
    lock();
    
    viewportSize = size;
    isViewportUpdated = true;
    
    unlock();
}

OESize OpenGLCanvas::getSize()
{
    OESize size;
    
    lock();
    
    switch (canvasType)
    {
        case OECANVAS_DISPLAY:
            size = displayConfiguration.displayResolution;
            
            break;
            
        case OECANVAS_PAPER:
            size = image.getSize();
            
            break;
            
        case OECANVAS_OPENGL:
            size = viewportSize;
            
            break;
    }
    
    unlock();
    
    return size;
}

OERect OpenGLCanvas::getClipRect()
{
    OERect rect;
    
    lock();
    
    switch (canvasType)
    {
        case OECANVAS_DISPLAY:
            rect.origin = OEMakePoint(0, 0);
            rect.size = displayConfiguration.displayResolution;
            
            break;
            
            
        case OECANVAS_PAPER:
        {
            float pixelDensityRatio = (paperConfiguration.pixelDensity.width /
                                       paperConfiguration.pixelDensity.height);
            float clipHeight = (viewportSize.height * image.getSize().width /
                                viewportSize.width / pixelDensityRatio);
            
            rect.origin = clipOrigin;
            rect.size = OEMakeSize(image.getSize().width,
                                   clipHeight);
            
            break;
        }
        case OECANVAS_OPENGL:
            rect.origin = OEMakePoint(0, 0);
            rect.size = viewportSize;
            
            break;
    }
    
    unlock();
    
    return rect;
}

void OpenGLCanvas::scrollPoint(OEPoint aPoint)
{
    lock();
    
    clipOrigin = aPoint;
    
    unlock();
}

OESize OpenGLCanvas::getPixelDensity()
{
    OESize pixelDensity;
    
    lock();
    
    switch (canvasType)
    {
        case OECANVAS_DISPLAY:
            pixelDensity = OEMakeSize(displayConfiguration.displayPixelDensity,
                                      displayConfiguration.displayPixelDensity);
            
            break;
            
        case OECANVAS_PAPER:
            pixelDensity = paperConfiguration.pixelDensity;
            
            break;
            
        case OECANVAS_OPENGL:
            pixelDensity = OEMakeSize(openGLConfiguration.pixelDensity,
                                      openGLConfiguration.pixelDensity);
            
            break;
    }
    
    unlock();
    
    return pixelDensity;
}

OESize OpenGLCanvas::getPageSize()
{
    OESize size;
    
    lock();
    
    switch (canvasType)
    {
        case OECANVAS_DISPLAY:
            size = displayConfiguration.displayResolution;
            
            break;
            
        case OECANVAS_PAPER:
            size = paperConfiguration.pageResolution;
            
            break;
            
        case OECANVAS_OPENGL:
            size = viewportSize;
            
            break;
    }
    
    unlock();
    
    return size;
}

OEImage OpenGLCanvas::getImage(OERect rect)
{
    if (canvasType == OECANVAS_PAPER)
        return OEImage(image, rect);
    
    OEImage image = readFramebuffer();
    
    OESize scale = OEMakeSize(image.getSize().height / getSize().height,
                              image.getSize().height / getSize().height);
    rect.origin.x *= scale.width;
    rect.origin.y *= scale.height;
    rect.size.width *= scale.width;
    rect.size.height *= scale.height;
    
    return OEImage(image, rect);
}

bool OpenGLCanvas::vsync()
{
    lock();
    
    CanvasVSync vSync;
    vSync.viewportSize = viewportSize;
    vSync.shouldDraw = false;
    
    if (isViewportUpdated)
    {
        isViewportUpdated = false;
        
        glViewport(0, 0, viewportSize.width, viewportSize.height);
        
        vSync.shouldDraw = true;
    }
    
    if (canvasType == OECANVAS_DISPLAY)
    {
        if (isImageUpdated)
            uploadImage();
        
        if (isConfigurationUpdated)
            configureShaders();
        
        if (isImageUpdated || isConfigurationUpdated)
        {
            isImageUpdated = false;
            isConfigurationUpdated = false;
            
            renderImage();
            
            vSync.shouldDraw = true;
        }
        
        if (displayConfiguration.displayPersistence != 0.0)
            vSync.shouldDraw = true;
    }
    else if (canvasType == OECANVAS_PAPER)
    {
        vSync.shouldDraw = isImageUpdated || isConfigurationUpdated;
    
        isImageUpdated = false;
        isConfigurationUpdated = false;
    }
    
    if (isBezelDrawRequired)
        vSync.shouldDraw = true;
    
    unlock();
    
    postNotification(this, CANVAS_DID_VSYNC, &vSync);
    
    if (vSync.shouldDraw)
        draw();
    
    return vSync.shouldDraw;
}

void OpenGLCanvas::draw()
{
    if (isViewportUpdated)
    {
        lock();
        
        glViewport(0, 0, viewportSize.width, viewportSize.height);
        
        isViewportUpdated = false;
        
        unlock();
    }
    
    if (canvasType == OECANVAS_DISPLAY)
    {
        lock();
        
        drawDisplayCanvas();
        
        drawBezel();
        
        unlock();
    }
    else if (canvasType == OECANVAS_PAPER)
    {
        lock();
        
        drawPaperCanvas();
        
        drawBezel();
        
        unlock();
    }
    else
    {
        postNotification(this, CANVAS_WILL_DRAW, &viewportSize);
        
        lock();
        
        drawBezel();
        
        unlock();
    }
}

// OpenGL

bool OpenGLCanvas::initOpenGL()
{
    for (OEInt i = 0; i < OPENGLCANVAS_TEXTUREEND; i++)
    {
        texture[i] = 0;
        textureSize[i] = OEMakeSize(0, 0);
    }
    
    isConfigurationUpdated = true;
    for (OEInt i = 0; i < OPENGLCANVAS_SHADEREND; i++)
        shader[i] = 0;
    
    capture = OPENGLCANVAS_CAPTURE_NONE;
    
    memset(keyDown, 0, sizeof(keyDown));
    keyDownCount = 0;
    ctrlAltWasPressed = false;
    mouseEntered = false;
    memset(mouseButtonDown, 0, sizeof(mouseButtonDown));
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glEnable(GL_TEXTURE_1D);
    glEnable(GL_TEXTURE_2D);
    
    glGenTextures(OPENGLCANVAS_TEXTUREEND, texture);
    
    loadTextures();
    
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    
    loadShaders();
    
    isOpen = true;
    
    return true;
}

void OpenGLCanvas::freeOpenGL()
{
    glDeleteTextures(OPENGLCANVAS_TEXTUREEND, texture);
    
    deleteShaders();
    
    isOpen = false;
}

GLuint OpenGLCanvas::getGLFormat(OEImageFormat format)
{
    if (format == OEIMAGE_LUMINANCE)
        return GL_LUMINANCE;
    else if (format == OEIMAGE_RGB)
        return GL_RGB;
    else if (format == OEIMAGE_RGBA)
        return GL_RGBA;
    
    return 0;
}

void OpenGLCanvas::loadTextures()
{
    loadTexture("images/Host/Shadow Mask Triad.png", true,
                OPENGLCANVAS_SHADOWMASK_TRIAD);
    loadTexture("images/Host/Shadow Mask Inline.png", true,
                OPENGLCANVAS_SHADOWMASK_INLINE);
    loadTexture("images/Host/Shadow Mask Aperture.png", true,
                OPENGLCANVAS_SHADOWMASK_APERTURE);
    loadTexture("images/Host/Shadow Mask LCD.png", true,
                OPENGLCANVAS_SHADOWMASK_LCD);
    loadTexture("images/Host/Shadow Mask Bayer.png", true,
                OPENGLCANVAS_SHADOWMASK_BAYER);
    loadTexture("images/Host/Bezel Power.png", false,
                OPENGLCANVAS_BEZEL_POWER);
    loadTexture("images/Host/Bezel Pause.png", false,
                OPENGLCANVAS_BEZEL_PAUSE);
    loadTexture("images/Host/Bezel Capture.png", false,
                OPENGLCANVAS_BEZEL_CAPTURE);
}

void OpenGLCanvas::loadTexture(string path, bool isMipmap, OEInt textureIndex)
{
    OEImage image;
    image.load(resourcePath + "/" + path);
    
    if (isMipmap)
    {
        glBindTexture(GL_TEXTURE_2D, texture[textureIndex]);
        
        gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB8,
                          image.getSize().width, image.getSize().height,
                          getGLFormat(image.getFormat()),
                          GL_UNSIGNED_BYTE, image.getPixels());
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, texture[textureIndex]);
        
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     image.getSize().width, image.getSize().height,
                     0,
                     getGLFormat(image.getFormat()), GL_UNSIGNED_BYTE, image.getPixels());
    }
    
    textureSize[textureIndex] = image.getSize();
}

void OpenGLCanvas::updateTextureSize(int textureIndex, OESize size)
{
    if (size.width < 4)
        size.width = 4;
    if (size.height < 1)
        size.height = 1;
    
    OESize texSize = OEMakeSize((float) getNextPowerOf2(size.width),
                                (float) getNextPowerOf2(size.height));
    
    if ((textureSize[textureIndex].width == texSize.width) &&
        (textureSize[textureIndex].height == texSize.height))
        return;
    
    textureSize[textureIndex] = texSize;
    
    vector<char> dummy;
    
    dummy.resize(texSize.width * texSize.height);
    
    glBindTexture(GL_TEXTURE_2D, texture[textureIndex]);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                 texSize.width, texSize.height,
                 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, &dummy.front());
}

void OpenGLCanvas::loadShaders()
{
    deleteShaders();
    
    loadShader(OPENGLCANVAS_RGB, rgbShader);
    loadShader(OPENGLCANVAS_COMPOSITE, compositeShader);
    loadShader(OPENGLCANVAS_DISPLAY, displayShader);
}

void OpenGLCanvas::deleteShaders()
{
    deleteShader(OPENGLCANVAS_RGB);
    deleteShader(OPENGLCANVAS_COMPOSITE);
    deleteShader(OPENGLCANVAS_DISPLAY);
}

void OpenGLCanvas::loadShader(GLuint shaderIndex, const char *source)
{
    GLuint index = 0;
#ifdef GL_VERSION_2_0
    
    const GLchar **sourcePointer = (const GLchar **) &source;
    GLint sourceLength = (GLint) strlen(source);
    
    GLuint glFragmentShader;
    glFragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(glFragmentShader, 1, sourcePointer, &sourceLength);
    glCompileShader(glFragmentShader);
    
    GLint bufSize;
    
    bufSize = 0;
    glGetShaderiv(glFragmentShader, GL_INFO_LOG_LENGTH, &bufSize);
    
    if (bufSize > 0)
    {
        GLsizei infoLogLength = 0;
        
        vector<char> infoLog;
        infoLog.resize(bufSize);
        
        glGetShaderInfoLog(glFragmentShader, bufSize,
                           &infoLogLength, &infoLog.front());
        
        string errorString = "could not compile OpenGL fragment shader " + getString(shaderIndex) + "\n";
        errorString += &infoLog.front();
        
        logMessage(errorString);
    }
    
    index = glCreateProgram();
    glAttachShader(index, glFragmentShader);
    glLinkProgram(index);
    
    glDeleteShader(glFragmentShader);
    
    bufSize = 0;
    glGetProgramiv(index, GL_INFO_LOG_LENGTH, &bufSize);
    
    if (bufSize > 0)
    {
        GLsizei infoLogLength = 0;
        
        vector<char> infoLog;
        infoLog.resize(bufSize);
        
        glGetProgramInfoLog(index, bufSize,
                            &infoLogLength, &infoLog.front());
        
        string errorString = "could not link OpenGL program " + getString(shaderIndex) + "\n";
        errorString += &infoLog.front();
        
        logMessage(errorString);
    }
#endif
    
    shader[shaderIndex] = index;
}

void OpenGLCanvas::deleteShader(GLuint shaderIndex)
{
#ifdef GL_VERSION_2_0
    if (shader[shaderIndex])
        glDeleteProgram(shader[shaderIndex]);
    
    shader[shaderIndex] = 0;
#endif
}

// Display canvas

bool OpenGLCanvas::uploadImage()
{
    // Upload image
    updateTextureSize(OPENGLCANVAS_IMAGE_IN, image.getSize());
    
    glBindTexture(GL_TEXTURE_2D, texture[OPENGLCANVAS_IMAGE_IN]);
    
    glTexSubImage2D(GL_TEXTURE_2D, 0,
                    0, 0,
                    image.getSize().width, image.getSize().height,
                    getGLFormat(image.getFormat()), GL_UNSIGNED_BYTE, image.getPixels());
    
    // Update configuration
    if ((image.getSampleRate() != imageSampleRate) ||
        (image.getBlackLevel() != imageBlackLevel) ||
        (image.getWhiteLevel() != imageWhiteLevel) ||
        (image.getSubcarrier() != imageSubcarrier))
    {
        imageSampleRate = image.getSampleRate();
        imageBlackLevel = image.getBlackLevel();
        imageWhiteLevel = image.getWhiteLevel();
        imageSubcarrier = image.getSubcarrier();
        
        isConfigurationUpdated = true;
    }
    
    // Upload phase info
    OEInt texSize = (OEInt) getNextPowerOf2((OEInt) image.getSize().height);
    
    vector<float> colorBurst = image.getColorBurst();
    vector<bool> phaseAlternation = image.getPhaseAlternation();
    
    vector<float> phaseInfo;
    phaseInfo.resize(3 * texSize);
    
    for (OEInt x = 0; x < image.getSize().height; x++)
    {
        float c = colorBurst[x % colorBurst.size()] / 2 / (float) M_PI;
        
        phaseInfo[3 * x + 0] = c - floorf(c);
        phaseInfo[3 * x + 1] = phaseAlternation[x % phaseAlternation.size()];
    }
    
    glBindTexture(GL_TEXTURE_1D, texture[OPENGLCANVAS_IMAGE_PHASEINFO]);
    
    glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB,
                 texSize,
                 0,
                 GL_RGB, GL_FLOAT, &phaseInfo.front());
    
    return true;
}

GLuint OpenGLCanvas::getRenderShader()
{
    switch (displayConfiguration.videoDecoder)
    {
        case CANVAS_RGB:
        case CANVAS_MONOCHROME:
            return shader[OPENGLCANVAS_RGB];
            
        case CANVAS_YUV:
        case CANVAS_YIQ:
        case CANVAS_CXA2025AS:
            return shader[OPENGLCANVAS_COMPOSITE];
    }
    
    return 0;
}

void OpenGLCanvas::configureShaders()
{
#ifdef GL_VERSION_2_0
    GLuint renderShader = getRenderShader();
    GLuint displayShader = shader[OPENGLCANVAS_DISPLAY];
    
    if (!renderShader || !displayShader)
        return;
    
    bool isCompositeDecoder = (renderShader == shader[OPENGLCANVAS_COMPOSITE]);
    
    // Render shader
    glUseProgram(renderShader);
    
    // Subcarrier
    if (isCompositeDecoder)
        glUniform1f(glGetUniformLocation(renderShader, "subcarrier"),
                    imageSubcarrier / imageSampleRate);
    
    // Filters
    OEVector w = OEVector::chebyshevWindow(17, 50);
    w = w.normalize();
    
    OEVector wy, wu, wv;
    
    float bandwidth = displayConfiguration.videoBandwidth / imageSampleRate;
    
    if (isCompositeDecoder)
    {
        float yBandwidth = displayConfiguration.videoLumaBandwidth / imageSampleRate;
        float uBandwidth = displayConfiguration.videoChromaBandwidth / imageSampleRate;
        float vBandwidth = uBandwidth;
        
        if (displayConfiguration.videoDecoder == CANVAS_YIQ)
            uBandwidth = uBandwidth + NTSC_IQ_DELTA / imageSampleRate;
        
        // Switch to video bandwidth when no subcarrier
        if ((imageSubcarrier == 0.0) ||
            (displayConfiguration.videoWhiteOnly))
        {
            yBandwidth = bandwidth;
            uBandwidth = bandwidth;
            vBandwidth = bandwidth;
        }
        
        wy = w * OEVector::lanczosWindow(17, yBandwidth);
        wy = wy.normalize();
        
        wu = w * OEVector::lanczosWindow(17, uBandwidth);
        wu = wu.normalize() * 2;
        
        wv = w * OEVector::lanczosWindow(17, vBandwidth);
        wv = wv.normalize() * 2;
    }
    else
    {
        wy = w * OEVector::lanczosWindow(17, bandwidth);
        wu = wv = wy = wy.normalize();
    }
    
    glUniform3f(glGetUniformLocation(renderShader, "c0"),
                wy.getValue(8), wu.getValue(8), wv.getValue(8));
    glUniform3f(glGetUniformLocation(renderShader, "c1"),
                wy.getValue(7), wu.getValue(7), wv.getValue(7));
    glUniform3f(glGetUniformLocation(renderShader, "c2"),
                wy.getValue(6), wu.getValue(6), wv.getValue(6));
    glUniform3f(glGetUniformLocation(renderShader, "c3"),
                wy.getValue(5), wu.getValue(5), wv.getValue(5));
    glUniform3f(glGetUniformLocation(renderShader, "c4"),
                wy.getValue(4), wu.getValue(4), wv.getValue(4));
    glUniform3f(glGetUniformLocation(renderShader, "c5"),
                wy.getValue(3), wu.getValue(3), wv.getValue(3));
    glUniform3f(glGetUniformLocation(renderShader, "c6"),
                wy.getValue(2), wu.getValue(2), wv.getValue(2));
    glUniform3f(glGetUniformLocation(renderShader, "c7"),
                wy.getValue(1), wu.getValue(1), wv.getValue(1));
    glUniform3f(glGetUniformLocation(renderShader, "c8"),
                wy.getValue(0), wu.getValue(0), wv.getValue(0));
    
    // Decoder matrix
    OEMatrix3 decoderMatrix(1, 0, 0,
                            0, 1, 0,
                            0, 0, 1);
    
    // Encode
    if (!isCompositeDecoder)
    {
        // Y'PbPr encoding matrix
        decoderMatrix = OEMatrix3(0.299F, -0.168736F, 0.5F,
                                  0.587F, -0.331264F, -0.418688F,
                                  0.114F, 0.5F, -0.081312F) * decoderMatrix;
    }
    
    // Set hue
    if (displayConfiguration.videoDecoder == CANVAS_MONOCHROME)
        decoderMatrix = OEMatrix3(1, 0.5F, 0,
                                  0, 0, 0,
                                  0, 0, 0) * decoderMatrix;
    
    // Disable color decoding when no subcarrier
    if (isCompositeDecoder)
    {
        if ((imageSubcarrier == 0.0) ||
            (displayConfiguration.videoWhiteOnly))
        {
            decoderMatrix = OEMatrix3(1, 0, 0,
                                      0, 0, 0,
                                      0, 0, 0) * decoderMatrix;
        }
    }
    
    // Saturation
    decoderMatrix = OEMatrix3(1, 0, 0,
                              0, displayConfiguration.videoSaturation, 0,
                              0, 0, displayConfiguration.videoSaturation) * decoderMatrix;
    
    // Hue
    float hue = 2 * (float) M_PI * displayConfiguration.videoHue;
    
    decoderMatrix = OEMatrix3(1, 0, 0,
                              0, cosf(hue), -sinf(hue),
                              0, sinf(hue), cosf(hue)) * decoderMatrix;
    
    // Decode
    switch (displayConfiguration.videoDecoder)
    {
        case CANVAS_RGB:
        case CANVAS_MONOCHROME:
            // Y'PbPr decoder matrix
            decoderMatrix = OEMatrix3(1, 1, 1,
                                      0, -0.344136F, 1.772F,
                                      1.402F, -0.714136F, 0) * decoderMatrix;
            break;
            
        case CANVAS_YUV:
        case CANVAS_YIQ:
            // Y'UV decoder matrix
            decoderMatrix = OEMatrix3(1, 1, 1,
                                      0, -0.394642F, 2.032062F,
                                      1.139883F, -0.580622F, 0) * decoderMatrix;
            break;
            
        case CANVAS_CXA2025AS:
            // Exchange I and Q
            decoderMatrix = OEMatrix3(1, 0, 0,
                                      0, 0, 1,
                                      0, 1, 0) * decoderMatrix;
            
            // Rotate 33 degrees
            hue = -(float) M_PI * 33 / 180;
            decoderMatrix = OEMatrix3(1, 0, 0,
                                      0, cosf(hue), -sinf(hue),
                                      0, sinf(hue), cosf(hue)) * decoderMatrix;
            
            // CXA2025AS decoder matrix
            decoderMatrix = OEMatrix3(1, 1, 1,
                                      1.630F, -0.378F, -1.089F,
                                      0.317F, -0.466F, 1.677F) * decoderMatrix;
            break;
    }
    
    // Brigthness
    float brightness = displayConfiguration.videoBrightness - imageBlackLevel;
    OEMatrix3 decoderOffset;
    
    if (isCompositeDecoder)
        decoderOffset = decoderMatrix * OEMatrix3(brightness, 0, 0,
                                                  0, 0, 0,
                                                  0, 0, 0);
    else
        decoderOffset = decoderMatrix * OEMatrix3(brightness, 0, 0,
                                                  brightness, 0, 0,
                                                  brightness, 0, 0);
    
    glUniform3f(glGetUniformLocation(renderShader, "decoderOffset"),
                decoderOffset.getValue(0, 0),
                decoderOffset.getValue(0, 1),
                decoderOffset.getValue(0, 2));
    
    // Contrast
    float contrast = displayConfiguration.videoContrast;
    
    float videoLevel = (imageWhiteLevel - imageBlackLevel);
    if (videoLevel > 0)
        contrast /= videoLevel;
    else
        contrast = 0;
    
    if (contrast < 0)
        contrast = 0;
    
    decoderMatrix *= contrast;
    
    glUniformMatrix3fv(glGetUniformLocation(renderShader, "decoderMatrix"),
                       1, false, decoderMatrix.getValues());
    
    // Display shader
    glUseProgram(displayShader);
    
    // Barrel
    glUniform1f(glGetUniformLocation(displayShader, "barrel"),
                displayConfiguration.displayBarrel);
    
    // Shadow mask
    glUniform1i(glGetUniformLocation(displayShader, "shadowMask"), 1);
    glUniform1f(glGetUniformLocation(displayShader, "shadowMaskLevel"),
                displayConfiguration.displayShadowMaskLevel);
    
    // Persistence
    float frameRate = 60;
    
    glUniform1f(glGetUniformLocation(displayShader, "persistenceLevel"),
                displayConfiguration.displayPersistence /
                (1.0F / frameRate + displayConfiguration.displayPersistence));
    
    if (displayConfiguration.displayPersistence == 0.0)
        updateTextureSize(OPENGLCANVAS_IMAGE_PERSISTENCE, OEMakeSize(0, 0));
    
    // Center lighting
    float centerLighting = displayConfiguration.displayCenterLighting;
    if (fabs(centerLighting) < 0.001)
        centerLighting = 0.001F;
    glUniform1f(glGetUniformLocation(displayShader, "centerLighting"),
                1.0F / centerLighting - 1);
    
    // Luminance gain
    glUniform1f(glGetUniformLocation(displayShader, "luminanceGain"),
                displayConfiguration.displayLuminanceGain);
    
    glUseProgram(0);
#endif
}

void OpenGLCanvas::renderImage()
{
#ifdef GL_VERSION_2_0
    GLuint renderShader = getRenderShader();
    
    if (!isShaderEnabled || !renderShader)
        return;
    
    bool isCompositeDecoder = (renderShader == shader[OPENGLCANVAS_COMPOSITE]);
    
    glUseProgram(renderShader);
    
    OESize texSize = textureSize[OPENGLCANVAS_IMAGE_IN];
    
    updateTextureSize(OPENGLCANVAS_IMAGE_DECODED, texSize);
    
    glUniform1i(glGetUniformLocation(renderShader, "texture"), 0);
    glUniform2f(glGetUniformLocation(renderShader, "textureSize"),
                texSize.width, texSize.height);
    
    if (isCompositeDecoder)
    {
        glUniform1i(glGetUniformLocation(renderShader, "phaseInfo"), 1);
        
        glActiveTexture(GL_TEXTURE1);
        
        glBindTexture(GL_TEXTURE_1D, texture[OPENGLCANVAS_IMAGE_PHASEINFO]);
        
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        
        glActiveTexture(GL_TEXTURE0);
    }
    
    // Render to the back buffer, to avoid using FBOs
    // (support for vanilla OpenGL 2.0 cards)
    glReadBuffer(GL_BACK);
    
    OESize imageSize = image.getSize();
    for (float y = 0; y < imageSize.height; y += viewportSize.height)
        for (float x = 0; x < imageSize.width; x += viewportSize.width)
        {
            // Calculate rects
            OESize clipSize = viewportSize;
            
            if ((x + clipSize.width) > imageSize.width)
                clipSize.width = imageSize.width - x;
            if ((y + clipSize.height) > imageSize.height)
                clipSize.height = imageSize.height - y;
            
            OERect textureRect = OEMakeRect(x / texSize.width,
                                            y / texSize.height,
                                            clipSize.width / texSize.width,
                                            clipSize.height / texSize.height);
            OERect canvasRect = OEMakeRect(-1,
                                           -1,
                                           2 * clipSize.width / viewportSize.width, 
                                           2 * clipSize.height / viewportSize.height);
            
            // Render
            glBindTexture(GL_TEXTURE_2D, texture[OPENGLCANVAS_IMAGE_IN]);
            
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            
            glLoadIdentity();
            
            glBegin(GL_QUADS);
            glTexCoord2f(OEMinX(textureRect), OEMinY(textureRect));
            glVertex2f(OEMinX(canvasRect), OEMinY(canvasRect));
            glTexCoord2f(OEMaxX(textureRect), OEMinY(textureRect));
            glVertex2f(OEMaxX(canvasRect), OEMinY(canvasRect));
            glTexCoord2f(OEMaxX(textureRect), OEMaxY(textureRect));
            glVertex2f(OEMaxX(canvasRect), OEMaxY(canvasRect));
            glTexCoord2f(OEMinX(textureRect), OEMaxY(textureRect));
            glVertex2f(OEMinX(canvasRect), OEMaxY(canvasRect));
            glEnd();
            
            // Copy framebuffer
            glBindTexture(GL_TEXTURE_2D, texture[OPENGLCANVAS_IMAGE_DECODED]);
            
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0,
                                x, y, 0, 0,
                                clipSize.width, clipSize.height);
        }
    
    glUseProgram(0);
#endif
}

// Convert screen coordinates to texture coordinates
OEPoint OpenGLCanvas::getDisplayCanvasTexPoint(OEPoint p)
{
    OEPoint videoCenter = displayConfiguration.videoCenter;
    OESize videoSize = displayConfiguration.videoSize;
    
    p = OEMakePoint((p.x - 2 * videoCenter.x) / videoSize.width,
                    (p.y - 2 * videoCenter.y) / videoSize.height);
    
    OESize imageSize = image.getSize();
    OESize texSize = textureSize[OPENGLCANVAS_IMAGE_IN];
    
    p.x = (p.x + 1) * 0.5F * imageSize.width / texSize.width;
    p.y = (p.y + 1) * 0.5F * imageSize.height / texSize.height;
    
    return p;
}

void OpenGLCanvas::drawDisplayCanvas()
{
    GLuint displayShader = shader[OPENGLCANVAS_DISPLAY];
    
    if (!isShaderEnabled)
        displayShader = 0;
    
    // Clear
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    if ((image.getSize().width == 0) ||
        (image.getSize().height == 0))
    {
        updateTextureSize(OPENGLCANVAS_IMAGE_PERSISTENCE, OEMakeSize(0, 0));
        
        return;
    }
    
    // Grab common variables
    OESize displayResolution = displayConfiguration.displayResolution;
    
    // Vertex rect
    OERect vertexRect = OEMakeRect(-1, -1, 2, 2);
    
    float viewportAspectRatio = viewportSize.width / viewportSize.height;
    float displayAspectRatio = displayResolution.width / displayResolution.height;
    
    float ratio = viewportAspectRatio / displayAspectRatio;
    
    if (ratio > 1)
    {
        vertexRect.origin.x /= ratio;
        vertexRect.size.width /= ratio;
    }
    else
    {
        vertexRect.origin.y *= ratio;
        vertexRect.size.height *= ratio;
    }
    
    // Base texture rect
    OERect baseTexRect = OEMakeRect(0, 0, 1, 1);
    
    // Canvas texture tect
    float interlaceShift = image.getInterlace() / image.getSize().height;
    
    OEPoint canvasTexLowerLeft = getDisplayCanvasTexPoint(OEMakePoint(-1, -1 + 2 * interlaceShift));
    OEPoint canvasTexUpperRight = getDisplayCanvasTexPoint(OEMakePoint(1, 1 + 2 * interlaceShift));
    
    OERect canvasTexRect = OEMakeRect(canvasTexLowerLeft.x,
                                      canvasTexLowerLeft.y,
                                      canvasTexUpperRight.x - canvasTexLowerLeft.x,
                                      canvasTexUpperRight.y - canvasTexLowerLeft.y);
    
    OESize canvasSize = OEMakeSize(0.5F * viewportSize.width *
                                   vertexRect.size.width,
                                   0.5F * viewportSize.height *
                                   vertexRect.size.height);
    
    OESize canvasVideoSize = OEMakeSize(canvasSize.width *
                                        displayConfiguration.videoSize.width,
                                        canvasSize.height *
                                        displayConfiguration.videoSize.height);
    
    OERect barrelTexRect;
    
    // Render
    OEInt textureIndex;
    if (displayShader)
        textureIndex = OPENGLCANVAS_IMAGE_DECODED;
    else
        textureIndex = OPENGLCANVAS_IMAGE_IN;
    
#ifdef GL_VERSION_2_0
    // Set uniforms
    if (displayShader)
    {
        glUseProgram(displayShader);
        
        // Texture
        OESize texSize = textureSize[textureIndex];
        
        glUniform1i(glGetUniformLocation(displayShader, "texture"), 0);
        glUniform2f(glGetUniformLocation(displayShader, "textureSize"),
                    texSize.width, texSize.height);
        
        // Barrel
        barrelTexRect = OEMakeRect(-0.5F, -0.5F / displayAspectRatio,
                                   1.0F, 1.0F / displayAspectRatio);
        glUniform2f(glGetUniformLocation(displayShader, "barrelSize"),
                    1, 1.0F / displayAspectRatio);
        
        // Scanlines
        float scanlineHeight = canvasVideoSize.height / image.getSize().height;
        float scanlineLevel = displayConfiguration.displayScanlineLevel;
        
        scanlineLevel = ((scanlineHeight > 2.5F) ? scanlineLevel :
                         (scanlineHeight < 2) ? 0 :
                         (scanlineHeight - 2) / (2.5F - 2) * scanlineLevel);
        
        glUniform1f(glGetUniformLocation(displayShader, "scanlineLevel"), scanlineLevel);
        
        // Shadow mask
        GLuint shadowMaskTexture = 0;
        float shadowMaskAspectRatio;
        switch (displayConfiguration.displayShadowMask)
        {
            case CANVAS_TRIAD:
                shadowMaskTexture = OPENGLCANVAS_SHADOWMASK_TRIAD;
                shadowMaskAspectRatio = 2 / (274.0F / 240.0F);
                break;
            case CANVAS_INLINE:
                shadowMaskTexture = OPENGLCANVAS_SHADOWMASK_INLINE;
                shadowMaskAspectRatio = 2;
                break;
            case CANVAS_APERTURE:
                shadowMaskTexture = OPENGLCANVAS_SHADOWMASK_APERTURE;
                shadowMaskAspectRatio = 2;
                break;
            case CANVAS_LCD:
                shadowMaskTexture = OPENGLCANVAS_SHADOWMASK_LCD;
                shadowMaskAspectRatio = 2;
                break;
            case CANVAS_BAYER:
                shadowMaskTexture = OPENGLCANVAS_SHADOWMASK_BAYER;
                shadowMaskAspectRatio = 2;
                break;
        }
        
        float shadowMaskDotPitch = displayConfiguration.displayShadowMaskDotPitch;
        
        if (shadowMaskDotPitch <= 0.001F)
            shadowMaskDotPitch = 0.001F;
        
        float shadowMaskElemX = (displayResolution.width /
                                 displayConfiguration.displayPixelDensity *
                                 25.4F * 0.5F / shadowMaskDotPitch);
        OESize shadowMaskSize = OEMakeSize(shadowMaskElemX,
                                           shadowMaskElemX * shadowMaskAspectRatio /
                                           displayAspectRatio);
        
        glActiveTexture(GL_TEXTURE1);
        
        glBindTexture(GL_TEXTURE_2D, texture[shadowMaskTexture]);
        
        glUniform2f(glGetUniformLocation(displayShader, "shadowMaskSize"),
                    shadowMaskSize.width, shadowMaskSize.height);
        
        // Persistence
        glActiveTexture(GL_TEXTURE2);
        
        glBindTexture(GL_TEXTURE_2D, texture[OPENGLCANVAS_IMAGE_PERSISTENCE]);
        
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        
        glActiveTexture(GL_TEXTURE0);
        
        glUniform1i(glGetUniformLocation(displayShader, "persistence"), 2);
        glUniform2f(glGetUniformLocation(displayShader, "persistenceOrigin"),
                    persistenceTexRect.origin.x, persistenceTexRect.origin.y);
        glUniform2f(glGetUniformLocation(displayShader, "persistenceSize"),
                    persistenceTexRect.size.width, persistenceTexRect.size.height);
    }
#endif
    
    glLoadIdentity();
    
    glRotatef(180, 1, 0, 0);
    
    glBindTexture(GL_TEXTURE_2D, texture[textureIndex]);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    
    // Render
    glBegin(GL_QUADS);
    glTexCoord2f(OEMinX(canvasTexRect), OEMinY(canvasTexRect));
#ifdef GL_VERSION_2_0
    if (displayShader)
        glMultiTexCoord2d(1, OEMinX(baseTexRect), OEMinY(baseTexRect));
#endif
    glVertex2f(OEMinX(vertexRect), OEMinY(vertexRect));
    
    glTexCoord2f(OEMaxX(canvasTexRect), OEMinY(canvasTexRect));
#ifdef GL_VERSION_2_0
    if (displayShader)
        glMultiTexCoord2d(1, OEMaxX(baseTexRect), OEMinY(baseTexRect));
#endif
    glVertex2f(OEMaxX(vertexRect), OEMinY(vertexRect));
    
    glTexCoord2f(OEMaxX(canvasTexRect), OEMaxY(canvasTexRect));
#ifdef GL_VERSION_2_0
    if (displayShader)
        glMultiTexCoord2d(1, OEMaxX(baseTexRect), OEMaxY(baseTexRect));
#endif
    glVertex2f(OEMaxX(vertexRect), OEMaxY(vertexRect));
    
    glTexCoord2f(OEMinX(canvasTexRect), OEMaxY(canvasTexRect));
#ifdef GL_VERSION_2_0
    if (displayShader)
        glMultiTexCoord2d(1, OEMinX(baseTexRect), OEMaxY(baseTexRect));
#endif
    glVertex2f(OEMinX(vertexRect), OEMaxY(vertexRect));
    glEnd();
    
    if (displayConfiguration.displayPersistence != 0.0)
    {
        updateTextureSize(OPENGLCANVAS_IMAGE_PERSISTENCE, viewportSize);
        
        glBindTexture(GL_TEXTURE_2D, texture[OPENGLCANVAS_IMAGE_PERSISTENCE]);
        
        glReadBuffer(GL_BACK);
        
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0,
                            0, 0, 0, 0,
                            viewportSize.width, viewportSize.height);
        
        OESize persistenceTexSize = OEMakeSize(viewportSize.width /
                                               textureSize[OPENGLCANVAS_IMAGE_PERSISTENCE].width,
                                               viewportSize.height /
                                               textureSize[OPENGLCANVAS_IMAGE_PERSISTENCE].height);
        persistenceTexRect = OEMakeRect((vertexRect.origin.x + 1) * 0.5F * persistenceTexSize.width,
                                        (vertexRect.origin.y + 1) * 0.5F * persistenceTexSize.height,
                                        vertexRect.size.width * 0.5F * persistenceTexSize.width,
                                        vertexRect.size.height * 0.5F * persistenceTexSize.height);
        
        persistenceTexRect.origin.y += persistenceTexRect.size.height;
        persistenceTexRect.size.height = -persistenceTexRect.size.height;
    }
    
#ifdef GL_VERSION_2_0
    if (displayShader)
        glUseProgram(0);
#endif
}

// Paper canvas

void OpenGLCanvas::drawPaperCanvas()
{
    // Clear
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    OESize imageSize = image.getSize();
    if (!imageSize.width || !imageSize.height)
        return;
    
    // Render
    float pixelDensityRatio = (paperConfiguration.pixelDensity.width /
                               paperConfiguration.pixelDensity.height);
    float canvasViewportHeight = (viewportSize.height * imageSize.width /
                                  viewportSize.width / pixelDensityRatio);
    OERect viewportCanvas = OEMakeRect(0,
                                       clipOrigin.y,
                                       paperConfiguration.pageResolution.width,
                                       canvasViewportHeight);
    
    OESize texSize = OEMakeSize((float) getNextPowerOf2(imageSize.width),
                                (float) getNextPowerOf2(PAPER_SLICE));
    updateTextureSize(OPENGLCANVAS_IMAGE_IN, texSize);
    
    glLoadIdentity();
    glRotatef(180, 1, 0, 0);
    
    OEInt startIndex = OEMinY(viewportCanvas) / PAPER_SLICE;
    OEInt endIndex = OEMaxY(viewportCanvas) / PAPER_SLICE;
    for (OEInt i = startIndex; i <= endIndex; i++)
    {
        OERect slice = OEMakeRect(0,
                                  i * PAPER_SLICE,
                                  imageSize.width,
                                  PAPER_SLICE);
        
        if (OEMinY(slice) >= imageSize.height)
            break;
        
        if (OEMaxY(slice) >= imageSize.height)
            slice.size.height = imageSize.height - slice.origin.y;
        
        glBindTexture(GL_TEXTURE_2D, texture[OPENGLCANVAS_IMAGE_IN]);
        
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        slice.size.width, slice.size.height,
                        getGLFormat(image.getFormat()), GL_UNSIGNED_BYTE,
                        image.getPixels() + image.getBytesPerRow() * (OEInt) OEMinY(slice));
        
        OERect textureRect = OEMakeRect(0, 0,
                                        OEWidth(slice) / texSize.width, OEHeight(slice) / texSize.height);
        OERect canvasRect = OEMakeRect(-1, 2 * (OEMinY(slice) - OEMinY(viewportCanvas)) / OEHeight(viewportCanvas) - 1,
                                       2, 2 * slice.size.height / OEHeight(viewportCanvas));
        
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        glBegin(GL_QUADS);
        glTexCoord2f(OEMinX(textureRect), OEMinY(textureRect));
        glVertex2f(OEMinX(canvasRect), OEMinY(canvasRect));
        glTexCoord2f(OEMaxX(textureRect), OEMinY(textureRect));
        glVertex2f(OEMaxX(canvasRect), OEMinY(canvasRect));
        glTexCoord2f(OEMaxX(textureRect), OEMaxY(textureRect));
        glVertex2f(OEMaxX(canvasRect), OEMaxY(canvasRect));
        glTexCoord2f(OEMinX(textureRect), OEMaxY(textureRect));
        glVertex2f(OEMinX(canvasRect), OEMaxY(canvasRect));
        glEnd();
    }
    
    // Render page separators
    glColor4f(0.8F, 0.8F, 0.8F, 1);
    
    OEInt pageNum = (OEInt) ceil(imageSize.height / paperConfiguration.pageResolution.height);
    for (OEInt i = 0; i < pageNum; i++)
    {
        OERect line = OEMakeRect(-1, 2 * (paperConfiguration.pageResolution.height *
                                          (i + 1) - OEMinY(viewportCanvas)) /
                                 OEHeight(viewportCanvas) - 1,
                                 2, 0);
        if (line.origin.y < -1)
            continue;
        if (line.origin.y > 1)
            break;
        
        glBegin(GL_LINES);
        glVertex2f(OEMinX(line), OEMinY(line));
        glVertex2f(OEMaxX(line), OEMaxY(line));
        glEnd();
    }
    
    glColor4f(1, 1, 1, 1);
}

// Bezel

double OpenGLCanvas::getCurrentTime()
{
    timeval time;
    
    gettimeofday(&time, NULL);
    
    return time.tv_sec + time.tv_usec * (1.0 / 1000000.0);
}

void OpenGLCanvas::drawBezel()
{
    GLuint textureIndex = 0;
    GLfloat textureAlpha = 1;
    GLfloat blackAlpha = 0;
    
    if (isBezelCapture)
    {
        double now = getCurrentTime();
        double diff = now - bezelCaptureTime;
        
        textureIndex = OPENGLCANVAS_BEZEL_CAPTURE;
        if (diff > (BEZELCAPTURE_DISPLAY_TIME +
                    BEZELCAPTURE_FADEOUT_TIME))
        {
            isBezelCapture = false;
            
            textureAlpha = 0;
        }
        else if (diff > BEZELCAPTURE_DISPLAY_TIME)
            textureAlpha = 0.5F + 0.5F * (float) cos((diff - BEZELCAPTURE_DISPLAY_TIME) *
                                                     M_PI / BEZELCAPTURE_FADEOUT_TIME);
    }
    else if (bezel == CANVAS_BEZEL_POWER)
    {
        textureIndex = OPENGLCANVAS_BEZEL_POWER;
        blackAlpha = 0.3F;
        isBezelDrawRequired = false;
    }
    else if (bezel == CANVAS_BEZEL_PAUSE)
    {
        textureIndex = OPENGLCANVAS_BEZEL_PAUSE;
        blackAlpha = 0.3F;
        isBezelDrawRequired = false;
    }
    else
        isBezelDrawRequired = false;
    
    if (!textureIndex)
        return;
    
    // Calculate rects
    OESize size = textureSize[textureIndex];
    
    OERect renderFrame = OEMakeRect(-(size.width + 0.5F) / viewportSize.width,
                                    -(size.height + 0.5F) / viewportSize.height,
                                    2 * size.width / viewportSize.width,
                                    2 * size.height / viewportSize.height);
    
    // Render
    glLoadIdentity();
    glRotatef(180, 1, 0, 0);
    
    glColor4f(0, 0, 0, blackAlpha);
    
    glBegin(GL_QUADS);
    glVertex2f(-1, -1);
    glVertex2f(1, -1);
    glVertex2f(1, 1);
    glVertex2f(-1, 1);
    glEnd();
    
    // Render bezel
    glBindTexture(GL_TEXTURE_2D, texture[textureIndex]);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    
    glColor4f(1, 1, 1, textureAlpha);
    
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex2f(OEMinX(renderFrame), OEMinY(renderFrame));
    glTexCoord2f(1, 0);
    glVertex2f(OEMaxX(renderFrame), OEMinY(renderFrame));
    glTexCoord2f(1, 1);
    glVertex2f(OEMaxX(renderFrame), OEMaxY(renderFrame));
    glTexCoord2f(0, 1);
    glVertex2f(OEMinX(renderFrame), OEMaxY(renderFrame));
    glEnd();
    
    glColor4f(1, 1, 1, 1);
}

// Read framebuffer

OEImage OpenGLCanvas::readFramebuffer()
{
    OERect canvasRect = OEMakeRect(0, 0, viewportSize.width, viewportSize.height);
    
    OESize displayResolution = displayConfiguration.displayResolution;
    
    float viewportAspectRatio = viewportSize.width / viewportSize.height;
    float displayAspectRatio = displayResolution.width / displayResolution.height;
    
    float ratio = viewportAspectRatio / displayAspectRatio;
    if (ratio > 1)
    {
        canvasRect.origin.x = canvasRect.size.width * ((1 - 1.0F / ratio) * 0.5F);
        canvasRect.size.width /= ratio;
    }
    else
    {
        canvasRect.origin.y = canvasRect.size.width * ((1 - ratio) * 0.5F);
        canvasRect.size.height *= ratio;
    }
    
    OEImage image;
    
    image.setFormat(OEIMAGE_RGB);
    image.setSize(canvasRect.size);
    
    glReadBuffer(GL_FRONT);
    
    glReadPixels(OEMinX(canvasRect), OEMinY(canvasRect),
                 OEWidth(canvasRect), OEHeight(canvasRect),
                 GL_RGB, GL_UNSIGNED_BYTE,
                 image.getPixels());
    
    return image;
}

// HID

void OpenGLCanvas::updateCapture(OpenGLCanvasCapture capture)
{
    //	logMessage("updateCapture");
    
    if (this->capture == capture)
        return;
    this->capture = capture;
    
    if (setCapture)
        setCapture(userData, capture);
}

void OpenGLCanvas::becomeKeyWindow()
{
}

void OpenGLCanvas::resignKeyWindow()
{
    resetKeysAndButtons();
    
    ctrlAltWasPressed = false;
    
    updateCapture(OPENGLCANVAS_CAPTURE_NONE);
}

void OpenGLCanvas::postHIDEvent(OEInt notification, OEInt usageId, float value)
{
    CanvasHIDEvent hidEvent;
    
    hidEvent.usageId = usageId;
    hidEvent.value = value;
    
    postNotification(this, notification, &hidEvent);
}

void OpenGLCanvas::sendUnicodeChar(CanvasUnicodeChar unicodeChar)
{
    //	logMessage("unicode " + getHexString(unicode));
    
    postHIDEvent(CANVAS_UNICODECHAR_WAS_SENT, unicodeChar, 0);
}

void OpenGLCanvas::setKey(OEInt usageId, bool value)
{
    if (keyDown[usageId] == value)
        return;
    
    keyDown[usageId] = value;
    keyDownCount += value ? 1 : -1;
    
    if ((keyDown[CANVAS_K_LEFTCONTROL] ||
         keyDown[CANVAS_K_RIGHTCONTROL]) &&
        (keyDown[CANVAS_K_LEFTALT] ||
         keyDown[CANVAS_K_RIGHTALT]))
        ctrlAltWasPressed = true;
    
    postHIDEvent(CANVAS_KEYBOARD_DID_CHANGE, usageId, value);
    
    if ((capture == OPENGLCANVAS_CAPTURE_KEYBOARD_AND_DISCONNECT_MOUSE_CURSOR) &&
        !keyDownCount && ctrlAltWasPressed)
    {
        ctrlAltWasPressed = false;
        
        double now = getCurrentTime();
        if ((now - bezelCaptureTime) < BEZELCAPTURE_DISPLAY_TIME)
            bezelCaptureTime = now - BEZELCAPTURE_DISPLAY_TIME;
        
        updateCapture(OPENGLCANVAS_CAPTURE_NONE);
    }
    
//    logMessage("key " + getHexString(usageId) + ": " + getString(value));
//    logMessage("keyDownCount " + getString(keyDownCount));
}

void OpenGLCanvas::enterMouse()
{
    mouseEntered = true;
    
    if (captureMode == CANVAS_CAPTURE_ON_MOUSE_ENTER)
        updateCapture(OPENGLCANVAS_CAPTURE_KEYBOARD_AND_HIDE_MOUSE_CURSOR);
    
    postHIDEvent(CANVAS_POINTER_DID_CHANGE, CANVAS_P_PROXIMITY, 1);
}

void OpenGLCanvas::exitMouse()
{
    mouseEntered = false;
    
    if (captureMode == CANVAS_CAPTURE_ON_MOUSE_ENTER)
        updateCapture(OPENGLCANVAS_CAPTURE_NONE);
    
    postHIDEvent(CANVAS_POINTER_DID_CHANGE, CANVAS_P_PROXIMITY, 0);
}

void OpenGLCanvas::setMousePosition(float x, float y)
{
    if (capture != OPENGLCANVAS_CAPTURE_KEYBOARD_AND_DISCONNECT_MOUSE_CURSOR)
    {
        postHIDEvent(CANVAS_POINTER_DID_CHANGE, CANVAS_P_X, x);
        postHIDEvent(CANVAS_POINTER_DID_CHANGE, CANVAS_P_Y, y);
    }
}

void OpenGLCanvas::moveMouse(float rx, float ry)
{
    if (capture == OPENGLCANVAS_CAPTURE_KEYBOARD_AND_DISCONNECT_MOUSE_CURSOR)
    {
        postHIDEvent(CANVAS_MOUSE_DID_CHANGE, CANVAS_M_RX, rx);
        postHIDEvent(CANVAS_MOUSE_DID_CHANGE, CANVAS_M_RY, ry);
    }
}

void OpenGLCanvas::sendMouseWheelEvent(OEInt index, float value)
{
    if (capture == OPENGLCANVAS_CAPTURE_KEYBOARD_AND_DISCONNECT_MOUSE_CURSOR)
        postHIDEvent(CANVAS_MOUSE_DID_CHANGE, CANVAS_M_WHEELX + index, value);
    else
        postHIDEvent(CANVAS_POINTER_DID_CHANGE, CANVAS_P_WHEELX + index, value);
}

void OpenGLCanvas::setMouseButton(OEInt index, bool value)
{
    if (index >= CANVAS_MOUSE_BUTTON_NUM)
        return;
    if (mouseButtonDown[index] == value)
        return;
    mouseButtonDown[index] = value;
    
    if (capture == OPENGLCANVAS_CAPTURE_KEYBOARD_AND_DISCONNECT_MOUSE_CURSOR)
        postHIDEvent(CANVAS_MOUSE_DID_CHANGE, CANVAS_M_BUTTON1 + index, value);
    else if ((captureMode == CANVAS_CAPTURE_ON_MOUSE_CLICK) &&
             (capture == OPENGLCANVAS_CAPTURE_NONE) &&
             (bezel == CANVAS_BEZEL_NONE) &&
             (index == 0) &&
             value)
    {
        isBezelDrawRequired = true;
        isBezelCapture = true;
        bezelCaptureTime = getCurrentTime();
        
        updateCapture(OPENGLCANVAS_CAPTURE_KEYBOARD_AND_DISCONNECT_MOUSE_CURSOR);
    }
    else
        postHIDEvent(CANVAS_POINTER_DID_CHANGE, CANVAS_P_BUTTON1 + index, value);
}

void OpenGLCanvas::resetKeysAndButtons()
{
    for (OEInt i = 0; i < CANVAS_KEYBOARD_KEY_NUM; i++)
        setKey(i, false);
    
    for (OEInt i = 0; i < CANVAS_MOUSE_BUTTON_NUM; i++)
        setMouseButton(i, false);
}

void OpenGLCanvas::doCopy(wstring& value)
{
    postNotification(this, CANVAS_DID_COPY, &value);
}

void OpenGLCanvas::doPaste(wstring value)
{
    postNotification(this, CANVAS_DID_PASTE, &value);
}

void OpenGLCanvas::doDelete()
{
    postNotification(this, CANVAS_DID_DELETE, NULL);
}

bool OpenGLCanvas::setCaptureMode(CanvasCaptureMode *value)
{
    if (captureMode == *value)
        return true;
    
    switch (*value)
    {
        case CANVAS_NO_CAPTURE:
            updateCapture(OPENGLCANVAS_CAPTURE_NONE);
            break;
            
        case CANVAS_CAPTURE_ON_MOUSE_CLICK:
            updateCapture(OPENGLCANVAS_CAPTURE_NONE);
            break;
            
        case CANVAS_CAPTURE_ON_MOUSE_ENTER:
            updateCapture(mouseEntered ? 
                          OPENGLCANVAS_CAPTURE_KEYBOARD_AND_HIDE_MOUSE_CURSOR : 
                          OPENGLCANVAS_CAPTURE_NONE);
            break;
        default:
            return false;
    }
    
    captureMode = *value;
    
    return true;
}

bool OpenGLCanvas::setBezel(CanvasBezel *value)
{
    if (bezel == *value)
        return true;
    
    lock();
    
    bezel = *value;
    isBezelDrawRequired = true;
    
    unlock();
    
    return true;
}

bool OpenGLCanvas::setDisplayConfiguration(CanvasDisplayConfiguration *value)
{
    lock();
    
    isConfigurationUpdated = true;
    displayConfiguration = *value;
    
    unlock();
    
    return true;
}

bool OpenGLCanvas::setPaperConfiguration(CanvasPaperConfiguration *value)
{
    lock();
    
    isConfigurationUpdated = true;
    paperConfiguration = *value;
    
    unlock();
    
    return true;
}

bool OpenGLCanvas::setOpenGLConfiguration(CanvasOpenGLConfiguration *value)
{
    lock();
    
    isConfigurationUpdated = true;
    openGLConfiguration = *value;
    
    unlock();
    
    return true;
}

bool OpenGLCanvas::postImage(OEImage *value)
{
    lock();
    
    switch (canvasType)
    {
        case OECANVAS_DISPLAY:
            image = *value;
            
            isImageUpdated = true;
            
            break;
            
        case OECANVAS_PAPER:
        {
            OESize srcSize = value->getSize();
            OESize destSize = image.getSize();
            
            if ((destSize.width == 0) || (destSize.height == 0))
            {
                image.setFormat(value->getFormat());
                
                destSize.width = 1;//paperConfiguration.pageResolution.width;
                destSize.height = 1;
            }
            
            OERect srcRect = OEMakeRect(printPosition.x, printPosition.y,
                                        srcSize.width, srcSize.height);
            OERect destRect = OEMakeRect(0, 0,
                                         destSize.width, destSize.height);
            OERect unionRect = OEUnionRect(srcRect, destRect);
            
            image.resize(unionRect.size, OEColor(255));
            
            image.blend(*value, printPosition, OEBLEND_MULTIPLY);
            
            isImageUpdated = true;
            
            break;
        }
        default:
            break;
    }
    
    unlock();
    
    return true;
}

bool OpenGLCanvas::clear()
{
    lock();
    
    switch (canvasType)
    {
        case OECANVAS_DISPLAY:
            image = OEImage();
            
            isImageUpdated = true;
            
            break;
            
        case OECANVAS_PAPER:
            image = OEImage();
            
            isImageUpdated = true;
            
            // To-Do: other stuff...
            break;
            
        default:
            break;
    }
    
    unlock();
    
    return true;
}

bool OpenGLCanvas::setPrintPosition(OEPoint *value)
{
    lock();
    
    printPosition = *value;
    
    unlock();
    
    return true;
}

CanvasKeyboardFlags OpenGLCanvas::getKeyboardFlags()
{
    CanvasKeyboardFlags flags = 0;
    
    OESetBit(flags, CANVAS_KF_LEFTCONTROL, keyDown[CANVAS_K_LEFTCONTROL]);
    OESetBit(flags, CANVAS_KF_LEFTSHIFT, keyDown[CANVAS_K_LEFTSHIFT]);
    OESetBit(flags, CANVAS_KF_LEFTALT, keyDown[CANVAS_K_LEFTALT]);
    OESetBit(flags, CANVAS_KF_LEFTGUI, keyDown[CANVAS_K_LEFTGUI]);
    OESetBit(flags, CANVAS_KF_RIGHTCONTROL, keyDown[CANVAS_K_RIGHTCONTROL]);
    OESetBit(flags, CANVAS_KF_RIGHTSHIFT, keyDown[CANVAS_K_RIGHTSHIFT]);
    OESetBit(flags, CANVAS_KF_RIGHTALT, keyDown[CANVAS_K_RIGHTALT]);
    OESetBit(flags, CANVAS_KF_RIGHTGUI, keyDown[CANVAS_K_RIGHTGUI]);
    
    return flags;
}

bool OpenGLCanvas::postMessage(OEComponent *sender, int message, void *data)
{
    switch (message)
    {
        case CANVAS_SET_CAPTUREMODE:
            return setCaptureMode((CanvasCaptureMode *)data);
            
        case CANVAS_SET_BEZEL:
            return setBezel((CanvasBezel *)data);
            
        case CANVAS_SET_KEYBOARD_LEDS:
            if (setKeyboardLEDs)
                setKeyboardLEDs(userData, *((CanvasKeyboardLEDs *)data));
            
            return true;
            
        case CANVAS_GET_KEYBOARD_FLAGS:
            *((CanvasKeyboardFlags *)data) = getKeyboardFlags();
            
            return true;
            
        case CANVAS_GET_KEYBOARD_ANYKEYDOWN:
            *((bool *)data) = (keyDownCount != 0);
            
            return true;
            
        case CANVAS_CONFIGURE_DISPLAY:
            return setDisplayConfiguration((CanvasDisplayConfiguration *)data);
            
        case CANVAS_CONFIGURE_PAPER:
            return setPaperConfiguration((CanvasPaperConfiguration *)data);
            
        case CANVAS_CONFIGURE_OPENGL:
            return setOpenGLConfiguration((CanvasOpenGLConfiguration *)data);
            
        case CANVAS_POST_IMAGE:
            return postImage((OEImage *)data);
            
        case CANVAS_CLEAR:
            return clear();
            
        case CANVAS_SET_PRINTPOSITION:
            return setPrintPosition((OEPoint *)data);
    }
    
    return false;
}

void OpenGLCanvas::postNotification(OEComponent *sender, int notification, void *data)
{
    lock();
    
    for (OEInt i = 0; i < observers[notification].size(); i++)
    {
        unlock();
        
        observers[notification][i]->notify(sender, notification, data);
        
        lock();
    }
    
    unlock();
}

bool OpenGLCanvas::addObserver(OEComponent *observer, int notification)
{
    lock();
    
    bool value = OEComponent::addObserver(observer, notification);
    
    unlock();
    
    return value;
}

bool OpenGLCanvas::removeObserver(OEComponent *observer, int notification)
{
    lock();
    
    bool value = OEComponent::removeObserver(observer, notification);
    
    unlock();
    
    return value;
}
