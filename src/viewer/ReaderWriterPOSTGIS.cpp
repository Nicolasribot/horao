#include "PostGisUtils.h"

#include <osgDB/FileNameUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osg/ShapeDrawable>
#include <osgUtil/Optimizer>

#include <iostream>
#include <sstream>
#include <algorithm>
#include <map>

// for RAII of connection
struct PostgisConnection
{
    PostgisConnection( const std::string & connInfo )
        : _conn( PQconnectdb(connInfo.c_str()) )
    {}

    operator bool(){ return CONNECTION_OK == PQstatus( _conn );  }

    ~PostgisConnection()
    {
        if (_conn) PQfinish( _conn );
    }

    // for RAII ok query results
    struct QueryResult
    {
        QueryResult( PostgisConnection & conn, const std::string & query )
            : _res( PQexec( conn._conn, query.c_str() ) )
            , _error( PQresultErrorMessage(_res) )
        {}

        ~QueryResult() 
        { 
            PQclear(_res);
        }

        operator bool() const { return _error.empty(); }

        PGresult * get(){ return _res; }

        const std::string & error() const { return _error; }

    private:
        PGresult * _res;
        const std::string _error;
        // non copyable
        QueryResult( const QueryResult & );
        QueryResult operator=( const QueryResult &); 
    };

private:
   PGconn * _conn;
};


struct ReaderWriterPOSTGIS : osgDB::ReaderWriter
{
    ReaderWriterPOSTGIS()
    {
        supportsExtension( "postgis", "PostGIS feature driver for osgEarth" );
        supportsExtension( "postgisd", "PostGIS feature driver for osgEarth" );
    }

    virtual const char* className()
    {
        return "PostGIS Feature Reader";
    }

    // note: stupid key="value" parser, value must not contain '"'  
    virtual ReadResult readNode(const std::string& file_name, const Options* ) const
    {
        if ( !acceptsExtension(osgDB::getLowerCaseFileExtension( file_name )))
            return ReadResult::FILE_NOT_HANDLED;

        std::cerr << "loaded plugin postgis for [" << file_name << "]\n";

        typedef std::map< std::string, std::string > AttributeMap;
        AttributeMap am;
        std::stringstream line(file_name);
        std::string key, value;
        while (    std::getline( line, key, '=' ) 
                && std::getline( line, value, '"' ) 
                && std::getline( line, value, '"' )){
            // remove spaces in key
            key.erase( remove_if(key.begin(), key.end(), isspace ), key.end());
            std::cout << "key=\"" << key << "\" value=\"" << value << "\"\n";
            am.insert( std::make_pair( key, value ) );
        }

        PostgisConnection conn( am["conn_info"] );
        if (!conn){
            std::cerr << "failed to open database with conn_info=\"" << am["conn_info"] << "\"\n";
            return ReadResult::FILE_NOT_FOUND;
        }

        PostgisConnection::QueryResult res( conn, am["query"].c_str() );
        if (!res){
            std::cerr << "failed to execute query=\"" <<  am["query"] << "\" : " << res.error() << "\n";
            return ReadResult::ERROR_IN_READING_FILE;
        }

        const int featureIdIdx = PQfnumber(res.get(), am["feature_id"].c_str() );
        if ( featureIdIdx < 0 )
        {
            std::cerr << "failed to obtain feature_id=\""<< am["feature_id"] <<"\"\n";
            return ReadResult::ERROR_IN_READING_FILE;
        }

        const int geomIdx = PQfnumber(res.get(),  am["geometry_column"].c_str() );
        if ( geomIdx < 0 )
        {
            std::cerr << "failed to obtain geometry_column=\""<< am["geometry_column"] <<"\"\n";
            return ReadResult::ERROR_IN_READING_FILE;
        }

        const int numFeatures = PQntuples( res.get() );
        std::cout << "got " << numFeatures << " features\n";
        

        // define transfo  layerToWord
        osg::Matrixd layerToWord;
        {
            osg::Vec3d center(0,0,0);
            if ( !( std::stringstream( am["center"] ) >> center.x() >> center.y() ) ){
                std::cerr << "failed to obtain center=\""<< am["center"] <<"\"\n";
                return ReadResult::ERROR_IN_READING_FILE;
            }
            layerToWord.makeTranslate( -center );
        }

        std::cout << "converting " << numFeatures << " features from postgis...\n";
        osg::ref_ptr<osg::Geode> group = new osg::Geode();

        osg::ref_ptr<osg::Geometry> multi = new osg::Geometry();

        multi->setUseVertexBufferObjects(true);

        osg::ref_ptr<osg::Vec3Array> vertices( new osg::Vec3Array );
        multi->setVertexArray( vertices.get() );
        osg::ref_ptr<osg::Vec3Array> normals( new osg::Vec3Array );
        multi->setNormalArray( normals.get() );
        multi->setNormalBinding( osg::Geometry::BIND_PER_VERTEX );
 
        const bool mergeGeometries = true;
        
        osg::Timer timer;
        timer.setStartTick();
        for( int i=0; i<numFeatures; i++ )
        {
            const char * wkb = PQgetvalue( res.get(), i, geomIdx );
            Stack3d::Viewer::Lwgeom lwgeom( wkb, Stack3d::Viewer::Lwgeom::WKB() );
            assert( lwgeom.get() );
            osg::ref_ptr<osg::Geometry> geom = Stack3d::Viewer::createGeometry( lwgeom.get(), layerToWord );
            assert( geom.get() );
            geom->setName( PQgetvalue( res.get(), i, featureIdIdx) );
            if (!mergeGeometries){
                group->addDrawable( geom.get() );
            }
            else {
                const int offset = vertices->size();
                const osg::Vec3Array * vtx = dynamic_cast<const osg::Vec3Array *>(geom->getVertexArray());
                const osg::Vec3Array * nrml = dynamic_cast<const osg::Vec3Array *>(geom->getNormalArray());
                assert(vtx && nrml);
                for ( size_t v=0; v < vtx->size(); v++ ) {
                   vertices->push_back( (*vtx)[v] );
                   normals->push_back( (*nrml)[v] );
                }
                // modify vtx indice of primitives
                for( size_t s=0; s<geom->getNumPrimitiveSets(); s++ ){
                    //int setWithSameMode = -1;
                    //for( size_t t=0; t<multi->getNumPrimitiveSets(); t++ ){
                    //    if ( geom->getPrimitiveSet(s)->getMode() == multi->getPrimitiveSet(s)->getMode() ){
                    //        setWithSameMode = t;
                    //        break;
                    //    }
                    //}

                    //if ( setWithSameMode < 0 ){
                        multi->addPrimitiveSet( Stack3d::Viewer::offsetIndices( geom->getPrimitiveSet(s), offset) );
                    //}
                    //else {

                    //}
                }
            }
        }
        std::cout << "time to convert " << timer.time_s() << "sec\n";

        if (mergeGeometries) group->addDrawable( multi.get() );

        timer.setStartTick();
        osgUtil::Optimizer optimizer;
        optimizer.optimize(group.get(), 
              //  osgUtil::Optimizer::ALL_OPTIMIZATIONS 
              //  osgUtil::Optimizer::REMOVE_REDUNDANT_NODES 
              //| osgUtil::Optimizer::TRISTRIP_GEOMETRY 
              osgUtil::Optimizer::MERGE_GEOMETRY 
              );
        std::cout << "time to optimize " << timer.time_s() << "sec\n";


 
        std::cout << "done\n";
//return ReadResult::ERROR_IN_READING_FILE;

        return group.release();
    }
};

REGISTER_OSGPLUGIN(postgis, ReaderWriterPOSTGIS)

