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

#define DEBUG_OUT if (1) std::cerr
#define ERROR (std::cerr << "error: ")

int crop(int i, int min, int max)
{
    return i > min ? ( i < max ? i : max ) : min;
}

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

        osg::ref_ptr<osg::HeightField> hf( new osg::HeightField() );
        hf->allocate( int( ( xmax-xmin )/meshSize ),  
                      int( ( ymax-ymin )/meshSize ) );
        hf->setXInterval( ( xmax-xmin ) / hf->getNumColumns() );
        hf->setYInterval( ( ymax-ymin ) / hf->getNumRows() );
        hf->setOrigin( osg::Vec3( xmin, ymin, 0 ) - origin );
        
        // we want gdal to give us an image with one pixel per grid vertex
        // gdal will decimate / duplicate pixels if needed
        // we need to fetch on pixels that are entierly in the mnt image
        // we compute the mnt pixels indices at the top left and bottom righ corners
        //
        const double tilePixelsBbox[4] = {
            xmin - 0.5*hf->getXInterval(),
            ymin - 0.5*hf->getYInterval(),
            xmax + 0.5*hf->getXInterval(),
            ymax + 0.5*hf->getYInterval()
        };
        const double mntBboxForTilePixels[4] = {
            originX + 0.5*hf->getXInterval(),
            originY - pixelHeight/pixelPerMetreY + 0.5*hf->getYInterval(),
            originX + pixelWidth/pixelPerMetreX - 0.5*hf->getXInterval(),
            originY - 0.5*hf->getYInterval()
        };
        const double intersection[4] = {
            std::max( mntBboxForTilePixels[0], tilePixelsBbox[0]),
            std::max( mntBboxForTilePixels[1], tilePixelsBbox[1]),
            std::min( mntBboxForTilePixels[2], tilePixelsBbox[2]),
            std::min( mntBboxForTilePixels[3], tilePixelsBbox[3]),
        };

        // now if the grid vertices is in this intersection, the grid pixel is
        // completely in the mnt image 

        // pixels centers are at xmin + i*grid_size
        const int gridPixelTopLeft[2] = {
            crop( std::ceil( (intersection[0] - xmin )/hf->getXInterval() ), 0, hf->getNumColumns() ),
            crop( std::ceil( ( ymax - intersection[3] )/hf->getYInterval() ), 0, hf->getNumRows() )
        };
        const int gridPixelBottomRight[2] = {
            crop( std::floor( (intersection[2] - xmin )/hf->getXInterval() ), 0, hf->getNumColumns() ),
            crop( std::floor( ( ymax - intersection[1] )/hf->getYInterval() ), 0, hf->getNumRows() )
        };

        // now get the pixels indexes of the mnt image for the top left corner
        // of the top left pixel
        const int mntPixelTopLeft[2] = {
             int( ( xmin + gridPixelTopLeft[0]*hf->getXInterval() - 0.5*hf->getXInterval() - originX )*pixelPerMetreX ),
             std::min( int( ( originY - ymax + gridPixelTopLeft[1]*hf->getYInterval() + 0.5*hf->getYInterval() )*pixelPerMetreY), pixelHeight-1)
        };

        const int mntPixelBottomRight[2] = {
             std::min( int( ( xmin + gridPixelBottomRight[0]*hf->getXInterval() + 0.5*hf->getXInterval() - originX )*pixelPerMetreX ), pixelWidth-1 ),
             int( ( originY - ymax + gridPixelBottomRight[1]*hf->getYInterval() - 0.5*hf->getYInterval() )*pixelPerMetreY )
        };

        GDALRasterBand* band = raster->GetRasterBand( 1 );
        GDALDataType dType = band->GetRasterDataType();
        int dSizeBits = GDALGetDataTypeSize( dType );
        const int w = gridPixelBottomRight[0] - gridPixelTopLeft[0] + 1;
        const int h = gridPixelBottomRight[1] - gridPixelTopLeft[1] + 1;
        // vector is automatically deleted, and data are contiguous
        std::vector<char> buffer(  w  *  h  *  dSizeBits / 8  );
        char* blockData = NULL;

        DEBUG_OUT << " intersection " 
            << intersection[0] << " "
            << intersection[1] << " "
            << intersection[2] << " "
            << intersection[3] << "\n";

        DEBUG_OUT << " grid " << hf->getNumColumns() << "x" << hf->getNumColumns() << "\n";
        DEBUG_OUT << " grid pixels " 
            << gridPixelTopLeft[0] << " "
            << gridPixelTopLeft[1] << " "
            << gridPixelBottomRight[0] << " "
            << gridPixelBottomRight[1] << "\n";

        DEBUG_OUT << " convert pixels " 
            << mntPixelTopLeft[0] << " "
            << mntPixelTopLeft[1] << " "
            << mntPixelBottomRight[0] << " "
            << mntPixelBottomRight[1] << "\n";
        DEBUG_OUT << " into image " << w << "x" << h << "\n";

        if ( buffer.size() ) {
            blockData = &buffer[0];
            band->RasterIO( GF_Read, 
                    mntPixelTopLeft[0], 
                    mntPixelTopLeft[1],
                    mntPixelBottomRight[0] - mntPixelTopLeft[0],
                    mntPixelBottomRight[1] - mntPixelTopLeft[1],
                    blockData, 
                    w, 
                    h, 
                    dType, 0, 0 );
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
        const int numRows = hf->getNumRows();
        for ( int i = gridPixelTopLeft[1]; i <= gridPixelBottomRight[1]; ++i ) {
            for ( int j = gridPixelTopLeft[0]; j <= gridPixelBottomRight[0]; ++j ) {

                assert(  i - gridPixelTopLeft[1] > 0 && i - gridPixelTopLeft[1] < h );
                assert(  j - gridPixelTopLeft[0] > 0 && j - gridPixelTopLeft[0] < w );
                const float z = float( ( SRCVAL( blockData, dType, 
                                ( i - gridPixelTopLeft[1] )*w + ( j - gridPixelTopLeft[0] ) ) 
                            * dataScale )  + dataOffset );
                assert( j >= 0 && j <= int(hf->getNumColumns()) ) ;
                assert( numRows - i >= 0 && numRows - i <= numRows ) ;

                hf->setHeight( j, numRows-i, z );
                zMax = std::max( z, zMax );
                if ( z > 300 ){ 
                    DEBUG_OUT << " z " << z << " at " << i << " " << j << "\n";
                    DEBUG_OUT << " z " << z << " at " 
                        << j - gridPixelTopLeft[0] << " " 
                        << i - gridPixelTopLeft[1] << "\n";
                }
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


