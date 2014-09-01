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
#include "SFosg.h"
#include "StringUtils.h"

#include <osgDB/FileNameUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgDB/ReadFile>
#include <osg/ShapeDrawable>
#include <osg/MatrixTransform>
#include <osgUtil/Optimizer>

#include <sstream>
#include <cassert>

#include <gdal/gdal_priv.h>
#include <gdal/cpl_conv.h>

#include <libpq-fe.h>

#define DEBUG_OUT if (0) std::cerr

//! for GDAL RAII
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

//! for postgres connection RAII
struct PostgisConnection {

    PostgisConnection( const std::string& connInfo )
        : _conn( PQconnectdb( connInfo.c_str() ) )
    {}

    operator bool() {
        return CONNECTION_OK == PQstatus( _conn );
    }

    ~PostgisConnection() {
        if ( _conn ) {
            PQfinish( _conn );
        }
    }

    // for RAII ok query results
    struct QueryResult {
        QueryResult( PostgisConnection& conn, const std::string& query )
            : _res( PQexec( conn._conn, query.c_str() ) )
            , _error( PQresultErrorMessage( _res ) )
        {}

        ~QueryResult() {
            PQclear( _res );
        }

        operator bool() const {
            return _error.empty();
        }

        PGresult* get() {
            return _res;
        }

        const std::string& error() const {
            return _error;
        }

    private:
        PGresult* _res;
        const std::string _error;
        // non copyable
        QueryResult( const QueryResult& );
        QueryResult operator=( const QueryResult& );
    };

private:
    PGconn* _conn;
};

void MyErrorHandler( CPLErr , int /*err_no*/, const char* msg )
{
    throw std::runtime_error( std::string( "from GDAL: " ) + msg );
}

struct ReaderWriterPOSTGIS : osgDB::ReaderWriter {
    ReaderWriterPOSTGIS() {
        GDALAllRegister();
        CPLSetErrorHandler( MyErrorHandler );
        supportsExtension( "postgis", "PostGIS feature loader" );
        supportsExtension( "postgisd", "PostGIS feature loader" );
    }

    const char* className() const {
        return "ReaderWriterPOSTGIS";
    }

    ReadResult readNode( std::istream&, const Options* ) const {
        return ReadResult::NOT_IMPLEMENTED;
    }

    //! @note stupid key="value" parser, value must not contain '"'
    ReadResult readNode( const std::string& file_name, const Options* options ) const {
        DEBUG_OUT << ( options ? options->getOptionString() : "options null ptr" ) << "\n";

        if ( !acceptsExtension( osgDB::getLowerCaseFileExtension( file_name ) ) ) {
            return ReadResult::FILE_NOT_HANDLED;
        }

        DEBUG_OUT << "loaded plugin postgis for [" << file_name << "]\n";

        osg::Timer timer;

        DEBUG_OUT << "connecting to postgis...\n";
        timer.setStartTick();

        std::stringstream line( file_name );
        AttributeMap am( line );

        PostgisConnection conn( am.value( "conn_info" ) );

        if ( !conn ) {
            std::cerr << "failed to open database with conn_info=\"" << am.value( "conn_info" ) << "\"\n";
            return ReadResult::FILE_NOT_FOUND;
        }

        DEBUG_OUT << "connected in " <<  timer.time_s() << "sec\n";

        DEBUG_OUT << "execute request...\n";
        timer.setStartTick();

        PostgisConnection::QueryResult res( conn, am.value( "query" ).c_str() );

        if ( !res ) {
            std::cerr << "failed to execute query=\"" <<  am.value( "query" ) << "\" : " << res.error() << "\n";
            return ReadResult::ERROR_IN_READING_FILE;
        }

        const int numFeatures = PQntuples( res.get() );

        DEBUG_OUT << "got " << numFeatures << " features in " << timer.time_s() << "sec\n";

        DEBUG_OUT << "converting " << numFeatures << " features from postgis...\n";

        timer.setStartTick();

        // define transfo  layerToWord
        osg::Matrixd layerToWord;

        osg::Vec3d origin;

        {
            if ( !( std::stringstream( am.value( "origin" ) ) >> origin.x() >> origin.y() >> origin.z() ) ) {
                std::cerr << "failed to obtain origin=\""<< am.value( "origin" ) <<"\"\n";
                return ReadResult::ERROR_IN_READING_FILE;
            }

            layerToWord.makeTranslate( -origin );
        }

        const std::string geocolumn = am.optionalValue( "geocolumn" ).empty() ? "geom" : am.value( "geocolumn" );

        const int geomIdx   = PQfnumber( res.get(),  geocolumn.c_str() );

        const int posIdx    = PQfnumber( res.get(),  "pos" );

        const int heightIdx = PQfnumber( res.get(),  "height" );

        const int widthIdx  = PQfnumber( res.get(),  "width" );

        osgGIS::Mesh mesh( layerToWord );

        if ( geomIdx >= 0 ) { // we have a geom column, we create the model from it
            for( int i=0; i<numFeatures; i++ ) {
                osgGIS::WKB wkb( PQgetvalue( res.get(), i, geomIdx ) );
                assert( wkb.get() );

                if ( !*wkb.get() ) {
                    continue;    // null value from postgres
                }

                mesh.push_back( wkb );
            }
        }
        else if ( posIdx >= 0 && heightIdx >= 0 && widthIdx >=0 ) { // we draw bars instead of geom
            for( int i=0; i<numFeatures; i++ ) {
                const float h = atof( PQgetvalue( res.get(), i, heightIdx ) );
                const float w = atof( PQgetvalue( res.get(), i, widthIdx ) );
                osgGIS::WKB wkb( PQgetvalue( res.get(), i, posIdx ) );
                assert( wkb.get() );

                if ( !*wkb.get() ) {
                    continue;    // null value from postgres
                }

                mesh.addBar( wkb, w, w, h );
            }
        }
        else {
            std::cerr << "cannot find either 'geom' column or 'height','width' columns\n";
            return ReadResult::ERROR_IN_READING_FILE;
        }

        osg::ref_ptr< osg::Geometry > geom = mesh.createGeometry();

        if ( !am.optionalValue( "elevation" ).empty() ) {
            Dataset raster( am.value( "elevation" ).c_str() );
            double transform[6];
            raster->GetGeoTransform( transform );
            const int pixelWidth = raster->GetRasterXSize();
            const int pixelHeight = raster->GetRasterYSize();

            // assume square pixels
            assert( std::abs( transform[4] ) < FLT_EPSILON );
            assert( std::abs( transform[2] ) < FLT_EPSILON );

            const double originX = transform[0];
            const double originY = transform[3];

            const double pixelPerMetreX =  1.f/transform[1];
            const double pixelPerMetreY = -1.f/transform[5]; // image is top->bottom
            GDALRasterBand* band = raster->GetRasterBand( 1 );
            GDALDataType dType = band->GetRasterDataType();
            const int dSizeBits = GDALGetDataTypeSize( dType );
            std::vector<char> buffer( dSizeBits / 8  );
            char* blockData = &buffer[0];

            double dataOffset;
            int ok;
            dataOffset = band->GetOffset( &ok );

            if ( ! ok ) {
                dataOffset = 0.0;
            }

            const double dataScale = band->GetScale( &ok );

            assert( ok );

            osg::Vec3Array* vtx = dynamic_cast<osg::Vec3Array*>( geom->getVertexArray() );

            assert( vtx );

            for ( osg::Vec3Array::iterator v = vtx->begin(); v!=vtx->end(); v++ ) {
                const int posX = int( ( v->x() + origin.x() - originX )*pixelPerMetreX );
                const int posY = int( ( originY - v->y() - origin.y() )*pixelPerMetreY );

                if ( posX >=0 && posX < pixelWidth && posY >= 0 && posY < pixelHeight ) {
                    band->RasterIO( GF_Read, posX, posY, 1, 1, blockData, 1, 1, dType, 0, 0 );
                    v->z() = float( ( SRCVAL( blockData, dType, 0 ) * dataScale )  + dataOffset ) - origin.z();
                }
            }
        }

        DEBUG_OUT << "converted " << numFeatures << " features in " << timer.time_s() << "sec\n";

        osg::ref_ptr<osg::Geode> group = new osg::Geode();
        group->addDrawable( geom.get() );
        return group.release();
    }
};

REGISTER_OSGPLUGIN( postgis, ReaderWriterPOSTGIS )


