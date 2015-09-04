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

#include <GL/glu.h>

extern "C" {
#include <liblwgeom.h>
}

#include <boost/format.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/visitors.hpp>
#include <boost/graph/undirected_dfs.hpp>
#include <boost/noncopyable.hpp>

#include <iostream>

// poly2tri gives better triangulation (delauny) than GLUtesselator
// in about twice the time (wich is really good)
// but since it's not robust, even with valid geometries (touching rings),
// we keep it for latter use
//#define POLY2TRI
#ifdef POLY2TRI
#include "../poly2tri/poly2tri.h"
#include <memory>
#include <iomanip>
#endif

namespace osgGIS {

//! custom error reporter for liblwgeom
inline
void errorreporter( const char* fmt, va_list ap )
{
    struct RaiiCharPtr {
        ~RaiiCharPtr() {
            free( ptr );
        }
        char* ptr;
    } msg;

    if ( !vasprintf ( &msg.ptr, fmt, ap ) ) {
        va_end ( ap );
        throw std::runtime_error( "unexpected error" );
    }

    va_end ( ap );
    throw std::runtime_error( std::string( "from liblwgeom: " )+msg.ptr );
}

// dummy class INSTANCE (this is a definition, look at the end!)
// to ensure initilization is done
const struct LwgeomInitialiser {
    LwgeomInitialiser() {
        lwgeom_set_handlers( NULL, NULL, NULL, errorreporter, NULL );
    }
} iAmJustAnInstanceSuchThatTheCtorIsExecuted;


// utility class for RAII of LWGEOM
struct Lwgeom {
    Lwgeom( WKT wkt )
        : _geom( lwgeom_from_wkt( wkt.get(), LW_PARSER_CHECK_NONE ) )
    {}
    Lwgeom( WKB wkb )
        : _geom( lwgeom_from_hexwkb( wkb.get(), LW_PARSER_CHECK_NONE ) )
    {}
    operator bool() const {
        return _geom;
    }
    const LWGEOM* get() const {
        return _geom;
    }
    const LWGEOM* operator->() const {
        return _geom;
    }
    ~Lwgeom() {
        if ( _geom ) {
            lwgeom_free( _geom );
        }
    }
private:
    LWGEOM* _geom;
};

// for debugging
inline
std::ostream& operator<<( std::ostream& o, const osg::Vec3& v )
{
    o << "( " << v.x() << ", " << v.y() << ", " << v.z() << " )";
    return o;
}


#ifdef POLY2TRI

struct Validity {
    /**
     * @note the class has private ctor to force the use of functions valid() and invalid(reason) that are clearer in the code than to remember that "Valid constructed with a reason is invalid"
     */
    static const Validity valid() {
        return Validity();
    }
    static const Validity invalid( const std::string& reason ) {
        return Validity( reason );
    }
    operator bool() const {
        return _valid;
    }
    const std::string& reason() const {
        return _reason;
    }
private:
    bool _valid; // not const to allow default copy
    std::string _reason;
    /**
     * @brief default ctor for valid
     */
    Validity():_valid( true ) {}
    /**
     * @brief if we construct with a reason, the class is invalid
     */
    Validity( const std::string& preason ):
        _valid( false ),
        _reason( preason )
    {}
};

// to detect unconnected interior in polygon
struct LoopDetector : public boost::dfs_visitor<> {
    LoopDetector( bool& hasLoop ):_hasLoop( hasLoop ) {}

    template <class Edge, class Graph>
    void back_edge( Edge, const Graph& ) {
        _hasLoop = true;
    }
private:
    bool& _hasLoop;
};


struct Ring : std::vector< osg::Vec3 > {};
struct Ring2d : std::vector< osg::Vec2 > {};

//! stores polygon with points converted to osg format
//! to avoid doing conversion several time
//! and prunes dupplicate points and remove last point
//! we keep the last point wich should be a duplicate of the first
struct Poly : boost::noncopyable {
    Poly( const LWPOLY* lwpoly, const osg::Matrix& layerToWord  )
        : rings( lwpoly->nrings ) {
        const size_t nrings = rings.size();

        for ( size_t r=0; r<nrings; r++ ) {
            const size_t npoints = lwpoly->rings[r]->npoints;
            rings[r].reserve( npoints );

            for ( size_t p=0; p<npoints; p++ ) {
                const POINT3DZ p3D = getPoint3dz( lwpoly->rings[r], p );
                const osg::Vec3 point =  osg::Vec3( p3D.x, p3D.y, p3D.z ) * layerToWord;

                if ( !p || rings[r].back() != point ) {
                    rings[r].push_back( point );
                }
            }
        }
    }

    std::vector< Ring > rings;
};

struct Poly2d : boost::noncopyable {
    // project Polygon on base
    Poly2d( const Poly& poly, const osg::Vec3 base[2] )
        : rings( poly.rings.size() ) {
        const size_t nrings = rings.size();

        for ( size_t r=0; r<nrings; r++ ) {
            const size_t npoints = poly.rings[r].size();
            rings[r].reserve( npoints );
            const osg::Vec3* srcRing = &( poly.rings[r][0] );

            for ( size_t p=0; p<npoints; p++ ) {
                rings[r].push_back( osg::Vec2( srcRing[p]*base[0], srcRing[p]*base[1] ) );
            }
        }
    }

    std::vector< Ring2d > rings;
};

inline
const osg::Vec3 normal( const Ring& ring )
{
    // Uses Newell's formula
    osg::Vec3 normal( 0.0, 0.0, 0.0 );

    const size_t npoints = ring.size() - 1;

    for ( size_t i = 0; i < npoints; ++i ) {
        const osg::Vec3 pi = ring[i];
        const osg::Vec3 pj = ring[ i+1 ];
        normal[0] += ( pi[1] - pj[1] ) * ( pi[2] + pj[2] );
        normal[1] += ( pi[2] - pj[2] ) * ( pi[0] + pj[0] );
        normal[2] += ( pi[0] - pj[0] ) * ( pi[1] + pj[1] );
    }

    normal.normalize();
    return normal;
}

inline
bool isPlane( const Poly& poly, const osg::Vec3* nrml = NULL, float epsilon = FLT_EPSILON )
{
    assert( poly.rings.size() );
    assert( poly.rings[0].size() );
    const osg::Vec3 first = poly.rings[0][0];

    const osg::Vec3 n = nrml ? *nrml : normal( poly.rings[0] );

    const size_t nrings = poly.rings.size();

    for ( size_t r=0; r<nrings; r++ ) {
        const osg::Vec3* ring = &( poly.rings[r][0] );
        const size_t npoints = poly.rings[r].size();

        for ( size_t i = 0; i < npoints; ++i ) {
            if ( ( ring[i] - first ) * n > epsilon ) {
                return false;
            }
        }
    }

    return true;
}

inline
bool isCovered( const osg::Vec2& point, const Ring2d& ring )
{
    // use ray casting algorithm
    // ray travels from point toward the positive y direction (constant x),
    // so crossing a boundary segment [start, end] means:
    // - start.x <= point.x < end.x || end.x < point.x <= start.x
    // - intersection of segment with the ray is above the point
    std::size_t intersectionCount = 0;
    const size_t npoints = ring.size();

    for ( size_t i = 0; i < npoints; ++i ) {
        const osg::Vec2 start = ring[i];
        const osg::Vec2 end = ring[ ( i+1 ) % npoints ];

        // special case with vertical segments aligned above the point
        // we don't consider crossing since it will cross the next segment its start
        // ence the <= or >= at start
        if ( start.x() != end.x()
                && ( ( ( start.x() <= point.x() ) && ( point.x() < end.x() ) ) || ( ( end.x() < point.x() ) && ( point.x() <= start.x() ) ) )
                && start.y() + ( ( point.x() - start.x() ) / ( end.x() - start.x() ) ) * ( end.y() - start.y() ) >= point.y()  ) {
            ++intersectionCount;
        }
    }

    return intersectionCount%2; // even -> outside
}

struct Segment2d: boost::noncopyable {
    Segment2d( const osg::Vec2& s, const osg::Vec2 e ): start( s ), end( e ) {}
    float length2() const {
        return ( end - start ).length2();
    }
    const osg::Vec2 start;
    const osg::Vec2 end;
};

inline
std::ostream& operator<<( std::ostream& o, const Segment2d& s )
{
    o << "[(" << s.start.x() << ", " << s.start.y() << ") , (" << s.end.x() << ", " << s.end.y() << ")]";
    return o;
}

struct Intersection {
    Intersection( const Segment2d& s1, const Segment2d& s2 ) {
        assert( s1.length2() > FLT_EPSILON );
        assert( s2.length2() > FLT_EPSILON );

        // the intersection occurs for unknown a and b such that
        // s1.start + a * ( s1.end - s1.start ) - s2.start - b * ( s2.end - s2.start ) = 0
        // that we put in matrix form
        // [ s1.end.x - s1.start.x     s2.start.x - s2.end.x ]( a ) = ( s2.start.x - s1.start.x )
        // [ s1.end.y - s1.start.y     s2.start.y - s2.end.y ]( b ) = ( s2.start.y - s1.start.y )
        // the solution, if any is then tested for a and b in [0,1]

        const float det = ( s1.end.x() - s1.start.x() ) * ( s2.start.y() - s2.end.y() )
                          - ( s2.start.x() - s2.end.x() ) * ( s1.end.y() - s1.start.y() );

        if ( std::abs( det ) < FLT_EPSILON ) { // parallel
            // test if alligned, solving for one direction only
            // s1.start + aStart * ( s1.end - s1.start ) = s2.start
            // s1.start + aEnd * ( s1.end - s1.start ) = s2.end

            const osg::Vec2 u = s1.end - s1.start;
            const osg::Vec2 v = s2.start - s1.start;

            if ( std::abs( u.x()*v.y() - u.y()*v.x() ) > FLT_EPSILON ) {
                return;    // points not aligned
            }

            const float denom = s1.end.x() - s1.start.x();

            const float aStart = std::abs( denom ) > FLT_EPSILON
                                 ? ( s2.start.x() - s1.start.x() ) / ( denom )
                                 : ( s2.start.y() - s1.start.y() ) / ( s1.end.y() - s1.start.y() );

            const float aEnd = std::abs( denom ) > FLT_EPSILON
                               ? ( s2.end.x() - s1.start.x() ) / ( denom )
                               : ( s2.end.y() - s1.start.y() ) / ( s1.end.y() - s1.start.y() );

            if ( aStart * aEnd < 0 // s1 lies on s2
                    || ( std::abs( aStart ) < FLT_EPSILON && std::abs( aEnd - 1.f ) < FLT_EPSILON ) ) { // s1 == s2
                _points.push_back( s1.start );
                _points.push_back(  s1.end );
            }
            else if ( aStart > 0 && aStart < 1.f ) {
                _points.push_back( osg::Vec2() ); // we dont care for first point
                _points.push_back( s1.start + ( s1.end -s1.start ) * aStart );
            }
            else if ( ( aEnd > 0 && aEnd < 1.f ) ) {
                _points.push_back( osg::Vec2() ); // we dont care for first point
                _points.push_back( s1.start + ( s1.end -s1.start ) * aEnd );
            }
            else if ( std::abs( aStart ) < FLT_EPSILON || std::abs( aEnd ) < FLT_EPSILON ) {
                _points.push_back( s1.start );
            }
            else if ( std::abs( aStart - 1.f ) < FLT_EPSILON || std::abs( aEnd - 1.f ) < FLT_EPSILON ) {
                _points.push_back( s1.end );
            }
        }
        else { // not parallel
            // solving a linear system here, watch out :)

            const float a = ( ( s2.start.x() - s1.start.x() ) * ( s2.start.y() - s2.end.y() )
                              - ( s2.start.x() - s2.end.x() ) * ( s2.start.y() - s1.start.y() ) )
                            / det;
            const float b = ( ( ( s1.end.x() - s1.start.x() ) * ( s2.start.y() - s1.start.y() ) )
                              - ( ( s2.start.x() - s1.start.x() ) * ( s1.end.y() - s1.start.y() ) ) )
                            / det;

            if ( a >= 0 && a <= 1.f && b >= 0 && b <= 1.f ) {
                _points.push_back( s1.start + ( s1.end -s1.start ) * a );
            }
        }
    }

    size_t dimension() const {
        return _points.size();
    }
    const osg::Vec2& point() const {
        assert( dimension() == 1 );
        return _points.front();
    }
private:
    std::vector< osg::Vec2 > _points;
};

inline
bool selfIntersects( const Ring2d& ring )
{
    // stupid O( n² ) algo
    const size_t npoints = ring.size() - 1;
    const size_t npointsMinus = npoints - 1;

    for ( size_t i=0; i<npointsMinus; i++ ) {
        const Segment2d s1( ring[i], ring[i+1] );

        for ( size_t j=i+2; j<npoints; j++ ) { // do not test neighbors
            const Segment2d s2( ring[j], ring[j+1] );
            const Intersection inter( s1, s2 );

            if ( inter.dimension() > 0  && !( inter.dimension() == 1 && 0 == i && ( npoints - 1 ) == j ) ) {
                return true;
            }
        }
    }

    return false;
}

const size_t INF = size_t( -1 );
//! @return the number of point itersections, INF if line intersection
inline
size_t nbIntersections( const Ring2d& ring1, const Ring2d& ring2 )
{

    // insert only points that are far enought from already inserted ones
    struct UniquePointSet: boost::noncopyable {
        void insert( const osg::Vec2& p ) {
            for ( size_t i=0; i<_points.size(); i++ ) {
                if ( ( _points[i] - p ).length2() < FLT_EPSILON ) {
                    return;
                }
            }

            _points.push_back( p );
        }

        size_t size() const {
            return _points.size();
        }
    private:
        std::vector<osg::Vec2> _points;
    };

    UniquePointSet set;

    // stupid O( n x m ) algo
    const size_t npoints1 = ring1.size() - 1;
    const size_t npoints2 = ring2.size() - 1;

    for ( size_t i=0; i<npoints1; i++ ) {
        const Segment2d s1( ring1[i], ring1[i+1] );

        for ( size_t j=0; j<npoints2; j++ ) {
            const Segment2d s2( ring2[j], ring2[j+1] );
            const Intersection inter( s1, s2 );

            if ( inter.dimension() == 1 ) {
                set.insert( inter.point() );
            }
            else if ( inter.dimension() == 2 ) {
                return INF;
            }
        }
    }

    //if (set.size() == 1) DEBUG_TRACE << "one contact point between two rings\n";
    return set.size();
}

// you get usefull output from this
inline
const Validity isValid( const Poly& poly, osg::Vec3 base[3], std::unique_ptr<Poly2d> & poly2d, bool fullCheck = true )
{
    // Closed simple rings (we test for simple a bit after)
    const size_t nrings =  poly.rings.size();

    if ( !nrings ) {
        return Validity::valid();    // empty is valid
    }

    for ( size_t r=0; r != nrings; ++r ) {
        if ( poly.rings[r].size() < 4 ) {
            return Validity::invalid( ( boost::format( "not enought points (%d) in ring %d" ) % poly.rings[r].size() % r ).str() );
        }

        if ( poly.rings[r].front() != poly.rings[r].back() ) {
            return Validity::invalid( ( boost::format( "ring %d is not closed" ) % r ).str() );
        }
    }

    base[2] = normal( poly.rings[0] );
    float norm2 = 0;
    // build a base to project the polygon onto
    {
        const osg::Vec3 origin = poly.rings[0][0];
        const size_t npoints = poly.rings[0].size() - 1;

        for ( size_t p=1; p<npoints; p++ ) {
            const osg::Vec3 candidate = poly.rings[0][p] - origin;

            if ( candidate.length2() > norm2 ) {
                base[0] = candidate;
                norm2 = candidate.length2();
            }
        }

        base[0].normalize();
        base[1] = base[2] ^ base[0];
    }
    const float epsilon = std::sqrt( norm2 ) * .001f;


    // should have a surface and ence an non null normal
    if ( base[2].length2() < FLT_EPSILON ) {
        return Validity::invalid( "zero surface (either degenerated to a point, or to a line, or alway up and alfway down)" );
    }

    // Orientation
    // Polygone must be planar (all points in the same plane)
    if ( !isPlane( poly, &base[2], epsilon ) ) {
        return Validity::invalid( "points don't lie in the same plane" );
    }

    // interior rings must be oriented opposit to exterior;
    for ( std::size_t r=1; r<nrings; ++r ) {
        if ( normal( poly.rings[r] ) * base[2] > 0 ) {
            return Validity::invalid( ( boost::format( "interior ring %d is oriented in the same direction as exterior ring" ) % r ).str() );
        }
    }


    // project polygon
    poly2d.reset( new Poly2d( poly, base ) );

    if ( fullCheck ) {
        // Test rings simplicity now
        for ( std::size_t r=0; r<nrings; ++r ) {
            if ( selfIntersects( poly2d->rings[r] ) ) {
                return Validity::invalid( ( boost::format( "ring %d self intersects" ) % r ).str() );
            }
        }

        // Rings must not share more than one point (no intersection)
        // the interior should be connected, so we build a graph of touching rings and detect loops
        {
            typedef std::pair<int,int> Edge;
            std::vector<Edge> touchingRings;

            for ( size_t ri=0; ri < nrings; ++ri ) { // no need for numRings-1, the next loop won't be entered for the last ring
                for ( size_t rj=ri+1; rj < nrings; ++rj ) {
                    const size_t nbInter = nbIntersections( poly2d->rings[ri], poly2d->rings[rj] );

                    if ( nbInter > 1 ) {
                        return Validity::invalid( ( boost::format( "intersection between ring %d and %d" ) % ri % rj ).str() );
                    }
                    else if ( nbInter == 1 ) {
                        touchingRings.push_back( Edge( ri,rj ) );
                    }
                }
            }

            {
                using namespace boost;
                typedef adjacency_list< vecS, vecS, undirectedS,
                        no_property,
                        property<edge_color_t, default_color_type> > Graph;
                typedef graph_traits<Graph>::vertex_descriptor vertex_t;

                Graph g( touchingRings.begin(), touchingRings.end(), nrings );

                bool hasLoop = false;
                LoopDetector vis( hasLoop );
                undirected_dfs( g, root_vertex( vertex_t( 0 ) ).visitor( vis ).edge_color_map( get( edge_color, g ) ) );

                if ( hasLoop ) {
                    return Validity::invalid( "interior is not connected" );
                }
            }
        }

        // Interior rings must be interior to exterior ring
        // since there is no crossing (tested above), just check that the firs point of int is coverd by ext
        for ( size_t r=1; r < nrings ; ++r ) {
            if ( ! isCovered( poly2d->rings[r][0], poly2d->rings[0] ) ) {
                return Validity::invalid( ( boost::format( "exterior ring doesn't cover interior ring %d" ) % r ).str() );
            }
        }

        // Interior ring must not cover one another
        // again, since there is no crossing, testing just two point, since one contact point is allowed
        // and it could be the first tested
        for ( size_t ri=1; ri < nrings; ++ri ) { // no need for numRings-1, the next loop won't be entered for the last ring
            for ( size_t rj=ri+1; rj < nrings; ++rj ) {
                if ( isCovered( poly2d->rings[rj][0], poly2d->rings[ri] ) && isCovered( poly2d->rings[rj][1], poly2d->rings[ri] ) ) {
                    return Validity::invalid( ( boost::format( "interior ring %d covers interior ring %d" ) % ri % rj ).str() );
                }
            }
        }
    }

    return Validity::valid();
}

void Mesh::push_back( const LWPOLY* lwpoly )
{
    assert( lwpoly );
    assert( lwpoly->nrings > 0 );

    Poly poly( lwpoly, _layerToWord );
    osg::Vec3 base[3];
    std::unique_ptr< Poly2d > poly2d;
    Validity validity( isValid( poly, base, poly2d ) );

    if ( !validity ) {
        //! todo draw lines for rings but no contour
        //wkt = lwgeom_to_wkt(lwpoly_as_lwgeom(lwpoly), WKT_EXTENDED, 16, NULL);
        //ERROR << "invalid polygon (" << validity.reason() << ") :" << wkt << "\n";
        //free(wkt);
        throw std::runtime_error( "invalid polygon (" + validity.reason() + ")" );
    }

    const osg::Vec3 normal = base[2];

    const float distance = normal * poly.rings[0][0];

    std::unique_ptr<p2t::CDT> cdt;

    const float dmax = 100.f;

    const float dmax2 = dmax*dmax;

    try {
        // retesselate
        const size_t nrings = poly2d->rings.size();

        for ( size_t r = 0; r < nrings; r++ ) {
            const int npoints = poly2d->rings[r].size() - 1;
            std::vector<p2t::Point*> polyline;
            polyline.reserve( npoints*1.3f );
            osg::Vec2 prev( 0,0 );

            for( int v = 0; v < npoints; v++ ) {
                const osg::Vec2 p =  poly2d->rings[r][v];
                const osg::Vec2 delta = p - prev;
                const float delta2 = delta.length2();

                if ( v && delta2 < 2*FLT_MIN ) {
                    continue;
                }

                if ( v && delta2 > dmax2 ) {
                    // interpolate points
                    const size_t nbAddedPt = std::sqrt( delta2 )/ dmax;

                    for ( size_t i=1; i<nbAddedPt; i++ ) {
                        const osg::Vec2 addedP = prev + delta*float( i )/nbAddedPt;
                        polyline.push_back( new p2t::Point( addedP.x(), addedP.y() ) );
                    }
                }

                polyline.push_back( new p2t::Point( p.x(), p.y() ) );
                prev = p;
            }

            if ( !r ) {
                cdt.reset( new p2t::CDT( polyline ) );
            }
            else {
                cdt->AddHole( polyline );
            }
        }

        cdt->Triangulate();
    }
    catch ( std::exception& e ) {
        throw std::runtime_error( std::string( "from poly2tri: " ) + e.what() );
    }

    std::vector<p2t::Triangle*> triangles( cdt->GetTriangles() );

    for ( size_t i = 0; i < triangles.size(); i++ ) {
        p2t::Triangle& t = *triangles[i];

        for ( int j=0; j<3; j++ ) {
            p2t::Point& a = *t.GetPoint( j );
            _tri.push_back( _vtx.size() );
            _vtx.push_back( base[0] * a.x + base[1] * a.y + base[2] * distance );
        }
    }

    _nrml.resize( _vtx.size(), normal );
}

#else
// nop callback
void CALLBACK noStripCB( GLboolean flag )
{
    //assert( flag );
    ( void )flag;
}

void CALLBACK tessBeginCB( GLenum which )
{
    //assert( which == GL_TRIANGLES );
    ( void )which;
}

void CALLBACK tessEndCB( GLenum which )
{
    //assert( which == GL_TRIANGLES );
    ( void )which;
}

void CALLBACK tessErrorCB( GLenum errorCode )
{
    const GLubyte* errorStr = gluErrorString( errorCode );
    throw std::runtime_error( reinterpret_cast<const char*>( errorStr ) );
}

void CALLBACK tessVertexCB( const GLdouble* vtx, void* data )
{
    // cast back to double type
    Mesh* that = ( Mesh* )data;
    that->_tri.push_back( that->_vtx.size() );
    that->_vtx.push_back( osg::Vec3( vtx[0], vtx[1], vtx[2] ) );
}

struct VecGL {
    GLdouble _comp[3];
};
std::list< VecGL > globalVtxForGlu;

void CALLBACK tessCombineCB( GLdouble coords[3], GLdouble* /*vertex_data*/[4], GLfloat /*weight*/[4], void** outData, void* /*data*/ )
{

    globalVtxForGlu.push_back( VecGL() );
    GLdouble* vertex = globalVtxForGlu.back()._comp;
    vertex[0] = coords[0];
    vertex[1] = coords[1];
    vertex[2] = coords[2];
    *outData = vertex;
}

// for RAII off GLUtesselator
struct Tessellator {
    Tessellator() {
        _tess = gluNewTess();
        gluTessProperty( _tess, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_POSITIVE );
        //gluTessProperty(_tess, GLU_TESS_BOUNDARY_ONLY, GL_TRUE);
        gluTessCallback( _tess, GLU_TESS_BEGIN, ( void (* )( void ) )tessBeginCB );
        gluTessCallback( _tess, GLU_TESS_END, ( void (* )( void ) )tessEndCB );
        gluTessCallback( _tess, GLU_TESS_ERROR, ( void (* )( void ) )tessErrorCB );
        gluTessCallback( _tess, GLU_TESS_VERTEX_DATA, ( void (* )() )tessVertexCB );
        gluTessCallback( _tess, GLU_TESS_EDGE_FLAG,  ( void (* )() )noStripCB );
        gluTessCallback( _tess, GLU_TESS_COMBINE_DATA,  ( void (* )() )tessCombineCB );
    }
    ~Tessellator() {
        gluDeleteTess( _tess );
    }

    GLUtesselator* _tess;
};

template<>
void Mesh::push_back( const LWPOLY* lwpoly )
{
    assert( lwpoly );

    const int numRings = lwpoly->nrings;

    if ( numRings == 0 ) {
        return;
    }

    size_t totalNumVtx = 0;

    for ( int r = 0; r < numRings; r++ ) {
        totalNumVtx += lwpoly->rings[r]->npoints;
    }

    std::vector< GLdouble > coord( totalNumVtx*3 );

    const size_t size = _tri.size();
    assert( _vtx.size() == size );

    try {
        // retesselate and add rings
        Tessellator tesselator ;
        gluTessBeginPolygon( tesselator._tess, this ); // with NULL data
        size_t currIdx = 0;

        for ( int r = 0; r < numRings; r++ ) {
            gluTessBeginContour( tesselator._tess );                    // outer quad
            const int ringSize = lwpoly->rings[r]->npoints;

            for( int v = 0; v < ringSize - 1; v++ ) {
                const POINT3DZ p3D = getPoint3dz( lwpoly->rings[r], v );
                const osg::Vec3 p = osg::Vec3( p3D.x, p3D.y, p3D.z ) * _layerToWord;
                coord[currIdx + 0] = p.x();
                coord[currIdx + 1] = p.y();
                coord[currIdx + 2] = p.z();
                gluTessVertex( tesselator._tess, &( coord[currIdx] ), &( coord[currIdx] ) );
                currIdx+=3;
            }

            gluTessEndContour( tesselator._tess );                    // outer quad
        }

        gluTessEndPolygon( tesselator._tess );
        globalVtxForGlu.clear(); // should keep allocated (reserved memory)
    }
    catch ( std::exception& e ) {
        std::cerr << "warnig: cannot tesselate polygon: " << e.what() << "\n";
        // undo modifications to _tri and _vtx
        _tri.resize( size );
        _vtx.resize( size );
        globalVtxForGlu.clear(); // should keep allocated (reserved memory)
    }


    //// Normal computation.
    ////
    //// We cannot accurately rely on triangles from the tessellation, since we could have
    //// very "degraded" triangles (close to a line), and the normal computation would be bad.
    //// In this case, we would have to average the normal vector over each triangle of the polygon.
    //// The Newell's formula is simpler and more direct here.
    osg::Vec3 normal( 0.0, 0.0, 0.0 );
    const int sz = lwpoly->rings[0]->npoints;

    for ( int i = 0; i < sz; ++i ) {
        const POINT3DZ p3Di = getPoint3dz( lwpoly->rings[0], i );
        const POINT3DZ p3Dj = getPoint3dz( lwpoly->rings[0], ( i+1 ) % sz );
        const osg::Vec3 pi= osg::Vec3( p3Di.x, p3Di.y, p3Di.z ) * _layerToWord;
        const osg::Vec3 pj= osg::Vec3( p3Dj.x, p3Dj.y, p3Dj.z ) * _layerToWord;
        normal[0] += ( pi[1] - pj[1] ) * ( pi[2] + pj[2] );
        normal[1] += ( pi[2] - pj[2] ) * ( pi[0] + pj[0] );
        normal[2] += ( pi[0] - pj[0] ) * ( pi[1] + pj[1] );
    }

    normal.normalize();

    if ( ( FLAGS_GET_Z( lwpoly->flags ) == 0 ) && ( normal[2] < 0 ) ) {
        // if this is a 2D surface and the normal is pointing down, reverse each new triangle
        normal[2] = 1;

        for ( size_t i = size/3; i < _tri.size() / 3; ++i ) {
            std::swap( _tri[3*i], _tri[3*i+2] );
        }
    }

    _nrml.resize( _vtx.size(), normal );

}
#endif



// we create the box triangles ourselves since an osg::Box for each feature is really slow
void Mesh::addBar( WKB center, float width, float depth, float height )
{

    Lwgeom lwgeom( center );

    if ( !lwgeom.get() ) {
        return;    // the error reporter takes care of errors
    }

    LWPOINT* lwpoint = lwgeom_as_lwpoint( lwgeom.get() );

    if( !lwpoint ) {
        throw std::runtime_error( "failed to get points from WKB" );
    }

    const POINT3DZ p = getPoint3dz( lwpoint->point, 0 );

    const osg::Vec3 ctr( p.x, p.y, p.z );

    // we build a bevelled box, without a bottom
    // it's base is centerd on origin

    const float x = .5f*width;

    const float y = .5f*depth;

    const float e = width/20;

    const unsigned o = unsigned( _vtx.size() );

    // vertex indices for base, top of the side faces and cap
    const unsigned b[8] = { o, o+1, o+2, o+3, o+4, o+5, o+6, o+7 };

    const unsigned t[8] = { o+8, o+9, o+10, o+11, o+12, o+13, o+14, o+15 };

    const unsigned c[4] = { o+16, o+17, o+18, o+19 };

    const osg::Vec3 vb[8] = {
        osg::Vec3( -x+e, -y  , 0 ),
        osg::Vec3( x-e, -y  , 0 ),
        osg::Vec3( x  , -y+e, 0 ),
        osg::Vec3( x  ,  y-e, 0 ),
        osg::Vec3( x-e,  y  , 0 ),
        osg::Vec3( -x+e,  y  , 0 ),
        osg::Vec3( -x  ,  y-e, 0 ),
        osg::Vec3( -x  , -y+e, 0 )
    };

    const osg::Vec3 nb[8] = {
        osg::Vec3( 0, -1, 0 ),
        osg::Vec3( 0, -1, 0 ),
        osg::Vec3( 1,  0, 0 ),
        osg::Vec3( 1,  0, 0 ),
        osg::Vec3( 0,  1, 0 ),
        osg::Vec3( 0,  1, 0 ),
        osg::Vec3( -1,  0, 0 ),
        osg::Vec3( -1,  0, 0 )
    };

    osg::Vec3 vt[8];

    for ( size_t i=0; i<8; i++ ) {
        vt[i] = vb[i] + osg::Vec3( 0,0,height-e );
    }


    const osg::Vec3 vc[4] = {
        osg::Vec3( -x+e, -y+e, height ),
        osg::Vec3( x-e, -y+e, height ),
        osg::Vec3( x-e,  y-e, height ),
        osg::Vec3( -x+e,  y-e, height )
    };

    const osg::Vec3 nc[4] = {
        osg::Vec3( 0, 0, 1 ),
        osg::Vec3( 0, 0, 1 ),
        osg::Vec3( 0, 0, 1 ),
        osg::Vec3( 0, 0, 1 )
    };

    _vtx.insert( _vtx.end(), vb, vb+8 );

    _nrml.insert( _nrml.end(), nb, nb+8 );

    _vtx.insert( _vtx.end(), vt, vt+8 );

    _nrml.insert( _nrml.end(), nb, nb+8 ); // same nrml as bottom

    _vtx.insert( _vtx.end(), vc, vc+4 );

    _nrml.insert( _nrml.end(), nc, nc+4 );

    // sides, loop / indices
    for ( size_t i=0; i<8; i++ ) {
        _tri.push_back( b[i] );
        _tri.push_back( b[( i+1 )%8] );
        _tri.push_back( t[i] );
        _tri.push_back( t[i] );
        _tri.push_back( b[( i+1 )%8] );
        _tri.push_back( t[( i+1 )%8] );
    }

    // top
    _tri.push_back( c[0] );
    _tri.push_back( c[1] );
    _tri.push_back( c[2] );
    _tri.push_back( c[0] );
    _tri.push_back( c[2] );
    _tri.push_back( c[3] );

    // top bevel
    for ( size_t i=0; i<4; i++ ) {
        _tri.push_back( t[( i*2 )%8] );
        _tri.push_back( t[( i*2+1 )%8] );
        _tri.push_back( c[i] );
        _tri.push_back( c[i] );
        _tri.push_back( t[( i*2+1 )%8] );
        _tri.push_back( c[( i+1 )%4] );
    }

    // top corners
    for ( size_t i=0; i<4; i++ ) {
        _tri.push_back( t[( i*2+1 )%8] );
        _tri.push_back( t[( i*2+2 )%8] );
        _tri.push_back( c[( i+1 )%4] );
    }

    // translate all vtx by base center
    const osg::Vec3 bc = ctr*_layerToWord;
    const size_t sz = _vtx.size();
    assert( _vtx.size() - o == 20 );

    for ( size_t i=o; i<sz; i++ ) {
        _vtx[i] += bc;
    }
}

template<>
void Mesh::push_back( const LWTRIANGLE* lwtriangle )
{
    assert( lwtriangle );
    const int offset = _vtx.size();

    for( int v = 0; v < 3; v++ ) {
        const POINT3DZ p3D = getPoint3dz( lwtriangle->points, v );
        const osg::Vec3d p( p3D.x, p3D.y, p3D.z );
        _vtx.push_back( p * _layerToWord );
    }

    for ( int i=0; i<3; i++ ) {
        _tri.push_back( i + offset );
    }

    osg::Vec3 normal( 0.0, 0.0, 0.0 );

    for ( int i = 0; i < 3; ++i ) {
        osg::Vec3 pi = _vtx[offset+i];
        osg::Vec3 pj = _vtx[offset +( ( i+1 ) % 3 ) ];
        normal[0] += ( pi[1] - pj[1] ) * ( pi[2] + pj[2] );
        normal[1] += ( pi[2] - pj[2] ) * ( pi[0] + pj[0] );
        normal[2] += ( pi[0] - pj[0] ) * ( pi[1] + pj[1] );
    }

    normal.normalize();

    if ( ( FLAGS_GET_Z( lwtriangle->flags ) == 0 ) && ( normal[2] < 0 ) ) {
        // if this is a 2D surface and the normal is pointing down, reverse the triangle
        normal[2] = 1;
        std::swap( _tri[ offset ], _tri[offset + 2] );
    }

    for ( int i=0; i<3; i++ ) {
        _nrml.push_back( normal );
    }
}

template< typename MULTITYPE >
void Mesh::push_back( const MULTITYPE* lwmulti )
{
    assert( lwmulti );
    const int numGeom = lwmulti->ngeoms;

    for ( int g = 0; g<numGeom; g++ ) {
        push_back( lwmulti->geoms[g] );
    }

}

template<>
void Mesh::push_back( const LWGEOM* lwgeom )
{
    assert( lwgeom );

    if ( lwgeom_is_empty( lwgeom ) ) {
        return;
    }

    //! @todo actually create the geometry
    switch ( lwgeom->type ) {
    case TRIANGLETYPE:
        push_back( lwgeom_as_lwtriangle( lwgeom ) );
        break;
    case TINTYPE:
        push_back( lwgeom_as_lwtin( lwgeom ) );
        break;
    case COLLECTIONTYPE:
        push_back( lwgeom_as_lwcollection( lwgeom ) );
        break;
    case MULTIPOLYGONTYPE:
        push_back( lwgeom_as_lwmpoly( lwgeom ) );
        break;
    case POLYHEDRALSURFACETYPE:
        push_back( lwgeom_as_lwpsurface( lwgeom ) );
        break;
    case POLYGONTYPE:
        push_back( lwgeom_as_lwpoly( lwgeom ) );
        break;
    case POINTTYPE:
        throw std::runtime_error( "POINTTYPE not handled" );
    case MULTIPOINTTYPE:
        throw std::runtime_error( "MULTIPOINTTYPE not handled" );
    case LINETYPE:
        throw std::runtime_error( "LINETYPE not handled" );
    case MULTILINETYPE:
        throw std::runtime_error( "MULTILINETYPE not handled" );
    case MULTISURFACETYPE:
        throw std::runtime_error( "MULTISURFACETYPE not handled" );
    case MULTICURVETYPE:
        throw std::runtime_error( "MULTICURVETYPE not handled" );
    case CIRCSTRINGTYPE:
        throw std::runtime_error( "CIRCSTRINGTYPE not handled" );
    case COMPOUNDTYPE:
        throw std::runtime_error( "COMPOUNDTYPE not handled" );
    case CURVEPOLYTYPE:
        throw std::runtime_error( "CURVEPOLYTYPE not handled" );
    }
}

void Mesh::push_back( WKT wkt )
{
    Lwgeom lwgeom( wkt );
    assert( lwgeom.get() ); // error reporter will take care of errors
    push_back( lwgeom.get() );
}

void Mesh::push_back( WKB wkb )
{
    Lwgeom lwgeom( wkb );
    assert( lwgeom.get() ); // error reporter will take care of errors
    push_back( lwgeom.get() );
}

osg::Geometry* Mesh::createGeometry() const
{
    osg::ref_ptr<osg::Geometry> multi = new osg::Geometry();
    multi->setUseVertexBufferObjects( true );

    osg::ref_ptr<osg::Vec3Array> vertices( new osg::Vec3Array( _vtx.begin(), _vtx.end() ) );
    multi->setVertexArray( vertices.get() );
    osg::ref_ptr<osg::Vec3Array> normals( new osg::Vec3Array( _nrml.begin(), _nrml.end() ) );
    multi->setNormalArray( normals.get() );
    multi->setNormalBinding( osg::Geometry::BIND_PER_VERTEX );

    osg::ref_ptr<osg::DrawElementsUInt> elem = new osg::DrawElementsUInt( GL_TRIANGLES, _tri.begin(), _tri.end() );
    multi->addPrimitiveSet( elem.get() );
    return multi.release();
}

}
