/**
 *   Horao
 *
 *   Copyright (C) 2013 Oslandia <infos@oslandia.com>
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Library General Public
 *   License as published by the Free Software Foundation; either
 *   version 2 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Library General Public License for more details.

 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "ViewerWidget.h"

#include <osg/CullFace>
#include <osg/Material>
#include <osgGA/KeySwitchMatrixManipulator>
#include <osgGA/TrackballManipulator>
#include <osgGA/TerrainManipulator>
#include <osgGA/OrbitManipulator>
#include <osgGA/StateSetManipulator>
#include <osgDB/ReadFile>
#include <osgText/Text>
#include <osg/io_utils>
#include <osg/Texture2D>

#include <cassert>
#include <stdexcept>
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 800
namespace Stack3d {
namespace Viewer {

const char* vertSourceSSAO = {
    "uniform vec2 g_Resolution;\n"
    "uniform float m_SubPixelShift;\n"
    "\n"
    "attribute vec4 inPosition;\n"
    "attribute vec2 inTexCoord;\n"
    "\n"
    "varying vec2 texCoord;\n"
    "varying vec4 posPos;\n"
    "\n"
    "void main() {\n"
    "gl_Position = inPosition * 2.0 - 1.0; //vec4(pos, 0.0, 1.0);\n"
    "texCoord = inTexCoord;\n"
    "vec2 rcpFrame = vec2(1.0) / g_Resolution;\n"
    "posPos.xy = inTexCoord.xy;\n"
    "posPos.zw = inTexCoord.xy - (rcpFrame * vec2(0.5 + m_SubPixelShift));\n"
    "}\n"
};

const char* fragSourceSSAO {
    "uniform vec2 g_Resolution;\n"
    "uniform sampler2D m_Texture;\n"
    "varying vec2 texCoord;\n"
    "void main() {\n"
    "gl_FragColor = texture2D(m_Texture, texCoord/g_Resolution) ;\n"
    "}\n"
    "\n"
};


ViewerWidget::ViewerWidget():
    osgViewer::Viewer()
{
    osg::setNotifyLevel( osg::NOTICE );

    //osg::DisplaySettings::instance()->setNumMultiSamples( 4 );

    setThreadingModel( osgViewer::Viewer::SingleThreaded );

    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    {
        osg::DisplaySettings* ds = osg::DisplaySettings::instance().get();
        traits->windowName = "Simple Viewer";
        traits->windowDecoration = true;
        traits->x = 0;
        traits->y = 0;
        traits->width = WINDOW_WIDTH;
        traits->height = WINDOW_HEIGHT;
        traits->doubleBuffer = true;
        traits->alpha = ds->getMinimumNumAlphaBits();
        traits->stencil = ds->getMinimumNumStencilBits();
        traits->sampleBuffers = ds->getMultiSamples();
        traits->samples = ds->getNumMultiSamples();
    }

    setUpViewInWindow( 0, 0, traits->width, traits->height );

    {
        osg::Camera* camera = getCamera();

        camera->setClearColor( osg::Vec4( 204.0/255, 204.0/255, 204.0/255, 1 ) );
        //camera->setViewport( new osg::Viewport( 0, 0, traits->width, traits->height ) );
        //camera->setProjectionMatrixAsPerspective( 30.0f, double( traits->width )/double( traits->height ), 1.0f, 100000.0f );
        //camera->setComputeNearFarMode(osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES);
        //camera->setComputeNearFarMode(osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES);

        _root = new osg::Group;


        // create sunlight
        //setLightingMode( osg::View::SKY_LIGHT );
        //getLight()->setPosition(osg::Vec4(1000,2000,3000,0));
        //getLight()->setDirection(osg::Vec3(-1,-2,-3));
        //getLight()->setAmbient(osg::Vec4( 0.6,0.6,0.6,1 ));
        //getLight()->setDiffuse(osg::Vec4( 0.6,0.6,0.6,1 ));
        //getLight()->setSpecular(osg::Vec4( 0.9,0.9,0.9,1 ));

        osg::ref_ptr<osgGA::KeySwitchMatrixManipulator> keyswitchManipulator = new osgGA::KeySwitchMatrixManipulator;

        keyswitchManipulator->addMatrixManipulator( '1', "Terrain", new osgGA::TerrainManipulator() );
        keyswitchManipulator->addMatrixManipulator( '2', "Orbit", new osgGA::OrbitManipulator() );
        keyswitchManipulator->addMatrixManipulator( '3', "Trackball", new osgGA::TrackballManipulator() );

        setCameraManipulator( keyswitchManipulator.get() );

        addEventHandler( new osgViewer::WindowSizeHandler );
        addEventHandler( new osgViewer::StatsHandler );
        addEventHandler( new osgGA::StateSetManipulator( getCamera()->getOrCreateStateSet() ) );
        addEventHandler( new osgViewer::ScreenCaptureHandler );

        setFrameStamp( new osg::FrameStamp );

        setSceneData( _root.get() );
    }

    // back alpha blending for "transparency"
    {
        osg::StateSet* ss = _root->getOrCreateStateSet();
        ss->setAttributeAndModes( new osg::CullFace( osg::CullFace::BACK ), osg::StateAttribute::ON );
        ss->setMode( GL_BLEND, osg::StateAttribute::ON );
    }


    // SSAO
    if( 0 ) {
        osg::StateSet* stateset = _root->getOrCreateStateSet();
        //osg::ref_ptr<osg::Program> program = new osg::Program;
        //program->addShader( new osg::Shader(osg::Shader::VERTEX, vertSourceSSAO) );
        //program->addShader( new osg::Shader(osg::Shader::FRAGMENT, fragSourceSSAO) );
        //stateset->setAttributeAndModes( program.get() );
        stateset->addUniform( new osg::Uniform( "m_SubPixelShift", .5f ) );
        stateset->addUniform( new osg::Uniform( "m_SpanMax", 5.f ) );
        stateset->addUniform( new osg::Uniform( "m_ReduceMul", 1.f ) );
        stateset->addUniform( new osg::Uniform( "g_Resolution", osg::Vec2( WINDOW_WIDTH, WINDOW_HEIGHT ) ) );
        osg::Texture2D* texture = new osg::Texture2D();
        texture->setTextureSize( WINDOW_WIDTH,WINDOW_HEIGHT );

        texture->setInternalFormat( GL_RGBA );

        texture->setWrap( osg::Texture2D::WRAP_S,osg::Texture2D::CLAMP_TO_EDGE );
        texture->setWrap( osg::Texture2D::WRAP_T,osg::Texture2D::CLAMP_TO_EDGE );
        texture->setFilter( osg::Texture2D::MIN_FILTER,osg::Texture2D::LINEAR );
        texture->setFilter( osg::Texture2D::MAG_FILTER,osg::Texture2D::LINEAR );

        osg::Camera* camera = new osg::Camera;
        camera->addChild( _root.get() );
        osg::Group* newRoot = new osg::Group;
        newRoot->addChild( camera );
        setSceneData( newRoot );
        camera->setRenderOrder( osg::Camera::PRE_RENDER );
        camera->setRenderTargetImplementation( osg::Camera::FRAME_BUFFER_OBJECT );
        //camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        camera->attach( osg::Camera::COLOR_BUFFER, texture );
        camera->setClearMask( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
        //camera->setProjectionMatrixAsOrtho2D(0, 0, WINDOW_HEIGHT, WINDOW_WIDTH);
        //setCameraManipulator( new osgGA::OrbitManipulator );

        stateset->addUniform( new osg::Uniform( "m_Texture", texture ) );

        // original image view
        if( 1 ) {

            //osg::Texture2D* tex = new osg::Texture2D;

            osg::Geode* geode = new osg::Geode();
            osg::Geometry* geom = new osg::Geometry();
            geode->addDrawable( geom );

            osg::Vec3Array* vtx = new osg::Vec3Array;
            vtx->push_back( osg::Vec3( 0, 0, 0 ) ); // front left
            vtx->push_back( osg::Vec3( WINDOW_WIDTH, 0, 0 ) ); // front right
            vtx->push_back( osg::Vec3( WINDOW_WIDTH, WINDOW_HEIGHT, 0 ) ); // back right
            vtx->push_back( osg::Vec3( 0,WINDOW_HEIGHT, 0 ) ); // back left
            geom->setVertexArray( vtx );

            osg::DrawElementsUInt* quads = new osg::DrawElementsUInt( osg::PrimitiveSet::QUADS, 0 );
            quads->push_back( 0 );
            quads->push_back( 1 );
            quads->push_back( 2 );
            quads->push_back( 3 );
            geom->addPrimitiveSet( quads );

            osg::Vec2Array* texcoords = new osg::Vec2Array( 5 );
            ( *texcoords )[0].set( 0.f,0.f ); // tex coord for vertex 0
            ( *texcoords )[1].set( 1.f,0.f ); // tex coord for vertex 1
            ( *texcoords )[2].set( 1.f,1.f ); // tex coord for vertex 1
            ( *texcoords )[3].set( 0.f,1.f ); // tex coord for vertex 1
            geom->setTexCoordArray( 0,texcoords );

            geode->getOrCreateStateSet()->setTextureAttributeAndModes( 0,texture,osg::StateAttribute::ON );
            osg::Camera* normalCamera = new osg::Camera();
            normalCamera->setClearColor( osg::Vec4( 0.4f,0.4f,0.4f,1.0f ) );
            normalCamera->setClearMask( GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT );
            normalCamera->setViewport( 0,0,WINDOW_WIDTH,WINDOW_HEIGHT );
            normalCamera->setProjectionMatrixAsOrtho2D( 0,WINDOW_HEIGHT,0,WINDOW_HEIGHT );
            normalCamera->setReferenceFrame( osg::Transform::ABSOLUTE_RF );
            normalCamera->setViewMatrix( osg::Matrix::identity() );
            normalCamera->setComputeNearFarMode( osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR );
            normalCamera->addChild( geode );
            //textureGroup->addChild( normalCamera );
            newRoot->addChild( normalCamera );
        }
    }

    realize();
}

osgGA::CameraManipulator* ViewerWidget::getCurrentManipulator()
{
    osgGA::CameraManipulator* manip = getCameraManipulator();
    osgGA::KeySwitchMatrixManipulator* kmanip = dynamic_cast< osgGA::KeySwitchMatrixManipulator* >( manip );

    if ( kmanip ) {
        return kmanip->getCurrentMatrixManipulator();
    }

    return manip;
}

void ViewerWidget::frame( double time )
{
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock( _mutex );
    osgViewer::Viewer::frame( time );
}

void ViewerWidget::setDone( bool flag ) volatile {
    ViewerWidget* that = const_cast< ViewerWidget* >( this );
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock( that->_mutex );
    that->osgViewer::Viewer::setDone( flag );
}


void ViewerWidget::setStateSet( const std::string& nodeId, osg::StateSet* stateset ) volatile {
    ViewerWidget* that = const_cast< ViewerWidget* >( this );
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock( that->_mutex );

    const NodeMap::const_iterator found = that->_nodeMap.find( nodeId );

    if ( found == that->_nodeMap.end() ) {
        throw std::runtime_error( "cannot find node '" + nodeId + "'" );
    }

    found->second->setStateSet( stateset );
}


void ViewerWidget::addNode( const std::string& nodeId, osg::Node* node ) volatile {
    ViewerWidget* that = const_cast< ViewerWidget* >( this );
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock( that->_mutex );

    if ( that->_nodeMap.find( nodeId ) != that->_nodeMap.end() ) {
        throw std::runtime_error( "node '" + nodeId + "' already exists" );
    }

    that->_root->addChild( node );
    that->_nodeMap.insert( std::make_pair( nodeId, node ) );
}

void ViewerWidget::removeNode(  const std::string& nodeId ) volatile {
    ViewerWidget* that = const_cast< ViewerWidget* >( this );
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock( that->_mutex );

    const NodeMap::const_iterator found = that->_nodeMap.find( nodeId );

    if ( found == that->_nodeMap.end() ) {
        throw std::runtime_error( "cannot find node '" + nodeId + "'" );
    }

    that->_root->removeChild( found->second.get() );
}

void ViewerWidget::setVisible( const std::string& nodeId, bool visible ) volatile {
    ViewerWidget* that = const_cast< ViewerWidget* >( this );
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock( that->_mutex );

    const NodeMap::const_iterator found = that->_nodeMap.find( nodeId );

    if ( found == that->_nodeMap.end() ) {
        throw std::runtime_error( "cannot find node '" + nodeId + "'" );
    }

    found->second->setNodeMask( visible ? 0xffffffff : 0x0 );
}

void ViewerWidget::setLookAt( const osg::Vec3& eye, const osg::Vec3& center, const osg::Vec3& up ) volatile {
    ViewerWidget* that = const_cast< ViewerWidget* >( this );
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock( that->_mutex );
    that->getCurrentManipulator()->setHomePosition( eye, center, up );
    that->getCurrentManipulator()->home( 0 );
}


void ViewerWidget::lookAtExtent( double xmin, double ymin, double xmax, double ymax ) volatile {
    ViewerWidget* that = const_cast< ViewerWidget* >( this );
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock( that->_mutex );
    double fovy, aspectRatio, zNear, zFar;

    if ( !that->getCamera()->getProjectionMatrixAsPerspective( fovy, aspectRatio, zNear, zFar ) ) {
        throw std::runtime_error( "cannot get projection matrix" );
    }

    // compute distance from fovy
    const double fovRad = fovy * M_PI / 180;
    const double altitude = .5*( ymax-ymin ) / std::tan( .5*fovRad );

    const osg::Vec3 up( 0,1,0 );
    const osg::Vec3 center( xmin+.5*( xmax-xmin ), ymin+.5*( ymax-ymin ), 0 );
    const osg::Vec3 eye( center.x(), center.y(), altitude );

    that->getCurrentManipulator()->setHomePosition( eye, center, up );
    that->getCurrentManipulator()->home( 0 );
}

void ViewerWidget::writeFile( const std::string& filename ) volatile {
    ViewerWidget* that = const_cast< ViewerWidget* >( this );
    OpenThreads::ScopedLock<OpenThreads::Mutex> lock( that->_mutex );

    if( !osgDB::writeNodeFile( *that->_root, filename ) ) {
        throw std::runtime_error( "cannot write '"+ filename + "'" );
    }
}

}
}

