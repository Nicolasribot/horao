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
#include "StringUtils.h"

#include <osgDB/FileNameUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgDB/ReadFile>
#include <osg/ShapeDrawable>
#include <osg/MatrixTransform>
#include <osg/PositionAttitudeTransform>
#include <osgUtil/Optimizer>
#include <osgTerrain/Terrain>

#include <iomanip>
#include <sstream>
#include <cassert>

#include <gdal_priv.h>
#include <cpl_conv.h>

#define DEBUG_OUT if (0) std::cerr
#define ERROR (std::cerr << "error: ")

void MyErrorHandler( CPLErr , int /*err_no*/, const char* msg )
{
    ERROR << "from GDAL:" << msg << "\n";
}

struct ReaderWriterMNT : osgDB::ReaderWriter {

    // for GDAL RAII
    struct Dataset {
        Dataset( const std::string& file )
            : _raster( ( GDALDataset* ) GDALOpen( file.c_str(), GA_ReadOnly ) )
        {}

        GDALDataset* operator->() {
            return _raster;
        }
        operator bool() {
            return _raster;
        }

        ~Dataset() {
            if ( _raster ) {
                GDALClose( _raster );
            }
        }
    private:
        GDALDataset* _raster;
    };

    ReaderWriterMNT() {
        GDALAllRegister();
        CPLSetErrorHandler( MyErrorHandler );

        supportsExtension( "mnt", "MNT tif loader" );
        supportsExtension( "mntd", "MNT tif loader" );
        DEBUG_OUT << "ctor of ReaderWriterMNT\n";
    }

    const char* className() const {
        return "ReaderWriterMNT";
    }

    ReadResult readNode( std::istream&, const Options* ) const {
        return ReadResult::NOT_IMPLEMENTED;
    }

    //! @note stupid key="value" parser, value must not contain '"'
    ReadResult readNode( const std::string& file_name, const Options* ) const {
        if ( !acceptsExtension( osgDB::getLowerCaseFileExtension( file_name ) ) ) {
            return ReadResult::FILE_NOT_HANDLED;
        }

        DEBUG_OUT << "loaded plugin mnt for [" << file_name << "]\n";

        osg::Timer timer;

        DEBUG_OUT << "loading...\n";
        timer.setStartTick();

        std::stringstream line( file_name );
        AttributeMap am( line );

        // define transfo  layerToWord
        //osg::Matrixd layerToWord;
        //{
        osg::Vec3d origin;

        if ( !( std::stringstream( am.value( "origin" ) ) >> origin.x() >> origin.y() >> origin.z() ) ) {
            ERROR << "failed to obtain origin=\"" << am.value( "origin" ) <<"\"\n";
            return ReadResult::ERROR_IN_READING_FILE;
        }

        //layerToWord.makeTranslate( -origin );
        //}

        double xmin, ymin, xmax, ymax;
        std::stringstream ext( am.value( "extent" ) );
        std::string l;

        if ( !( ext >> xmin >> ymin )
                || !std::getline( ext, l, ',' )
                || !( ext >> xmax >> ymax ) ) {
            ERROR << "cannot parse extent=\"" << am.value( "extent" ) << "\"\n";;
            return ReadResult::ERROR_IN_READING_FILE;
        }

        if ( xmin > xmax || ymin > ymax ) {
            ERROR << "cannot parse extent=\"" << am.value( "extent" ) << "\" xmin must be inferior to xmax and ymin to ymax in extend=\"min ymin,xmax ymx\"\n";;
            return ReadResult::ERROR_IN_READING_FILE;
        }

        double meshSize;

        if ( !( std::istringstream( am.value( "mesh_size" ) ) >> meshSize ) ) {
            ERROR << "cannot parse mesh_size=\"" << am.value( "mesh_size" ) << "\"\n";
            return ReadResult::ERROR_IN_READING_FILE;
        }

        Dataset raster( am.value( "file" ).c_str() );

        if ( ! raster ) {
            ERROR << "cannot open dataset from file=\"" << am.value( "file" ) << "\"\n";
            return ReadResult::ERROR_IN_READING_FILE;
        }

        if ( raster->GetRasterCount() < 1 ) {
            ERROR << "invalid number of bands\n";
            return ReadResult::ERROR_IN_READING_FILE;
        }

        const int pixelWidth = raster->GetRasterXSize();

        const int pixelHeight = raster->GetRasterYSize();

        double transform[6];

        raster->GetGeoTransform( transform );

        // assume square pixels
        assert( std::abs( transform[4] ) < FLT_EPSILON );

        assert( std::abs( transform[2] ) < FLT_EPSILON );

        const double originX = transform[0];

        const double originY = transform[3];

        const double pixelPerMetreX =  1.f/transform[1];

        const double pixelPerMetreY = -1.f/transform[5]; // image is top->bottom

        assert( pixelPerMetreX > 0 && pixelPerMetreY > 0 );

        const int Lx = std::max( 1, int( meshSize * pixelPerMetreX ) ) ;

        const int Ly = std::max( 1, int( meshSize * pixelPerMetreY ) ) ;

        // compute the position of the tile
        int x= ( xmin - originX ) * pixelPerMetreX ;

        int y= ( originY - ymax ) * pixelPerMetreY ;

        int w= ( xmax - xmin ) * pixelPerMetreX / Lx ;

        int h= ( ymax - ymin ) * pixelPerMetreY / Ly;

        // resize to fit data (avoid out of bound)
        if ( y < 0 ) {
            h = std::max( 0, h+y );
            y=0;
        }

        if ( y + h > pixelHeight ) {
            h = std::max( 0, pixelHeight - y );
        }

        if ( x < 0 ) {
            w = std::max( 0, w+x );
            x=0;
        }

        if ( x + w > pixelWidth ) {
            w = std::max( 0, pixelWidth - x );
        }


        DEBUG_OUT << std::setprecision( 8 ) << " xmin=" << xmin << " ymin=" << ymin << " xmax=" << xmax << " ymax=" << ymax << "\n";
        DEBUG_OUT << " originX=" << originX << " originY=" << originY << " pixelWidth=" << pixelWidth << " pixelHeight=" << pixelHeight
                  << " pixelPerMetreX=" << pixelPerMetreX
                  << " pixelPerMetreY=" << pixelPerMetreY
                  << "\n";
        DEBUG_OUT << " x=" << x << " y=" << y << " w=" << w << " h=" << h << " Lx=" << Lx  << " Ly=" << Ly << "\n";

        assert( h >= 0 && w >= 0 );

        osg::ref_ptr<osg::HeightField> hf( new osg::HeightField() );

        hf->allocate( w, h );
        hf->setXInterval( ( xmax-xmin )/( w-1 ) );
        hf->setYInterval( ( ymax-ymin )/( h-1 ) );
        hf->setOrigin( osg::Vec3( xmin, ymin, 0 ) - origin );

        GDALRasterBand* band = raster->GetRasterBand( 1 );
        GDALDataType dType = band->GetRasterDataType();
        int dSizeBits = GDALGetDataTypeSize( dType );
        // vector is automatically deleted, and data are contiguous
        std::vector<char> buffer( w * h * dSizeBits / 8  );
        char* blockData = &buffer[0];

        if ( buffer.size() ) {
            band->RasterIO( GF_Read, x, y, w * Lx, h * Ly, blockData, w, h, dType, 0, 0 );
        }

        double dataOffset;
        double dataScale;
        int ok;
        dataOffset = band->GetOffset( &ok );

        if ( ! ok ) {
            dataOffset = 0.0;
        }

        dataScale = band->GetScale( &ok );

        if ( ! ok ) {
            ERROR << "cannot get scale\n";
            dataScale = 1.0;
        }

        float zMax = 0;

        for ( int i = 0; i < h; ++i ) {
            for ( int j = 0; j < w; ++j ) {
                const float z = float( ( SRCVAL( blockData, dType, i*w+j ) * dataScale )  + dataOffset );
                hf->setHeight( j, h-1-i, z );
                zMax = std::max( z, zMax );
            }
        }

        DEBUG_OUT << "zMax=" << zMax << "\n";

        hf->setSkirtHeight( ( xmax-xmin )/10 );

        DEBUG_OUT << "loaded in " << timer.time_s() << "sec\n";

        osg::Geode* geode = new osg::Geode;
        geode->addDrawable( new osg::ShapeDrawable( hf.get() ) );
        return geode;
    }
};

REGISTER_OSGPLUGIN( postgis, ReaderWriterMNT )


