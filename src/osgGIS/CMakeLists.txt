add_library( osgdb_postgis MODULE 
    ReaderWriterPOSTGIS.cpp 
    SFosg.cpp
)
set_target_properties( osgdb_postgis PROPERTIES DEBUG_POSTFIX "d" )
set_target_properties( osgdb_postgis PROPERTIES PREFIX "")
# --Nicolasribot: adds target library for libpq
# ld error on OSX El Capitan: 
#[ 95%] Linking CXX shared module ../../lib/osgdb_postgis.so
#Undefined symbols for architecture x86_64:
#  "_PQclear", referenced from:
#      ReaderWriterPOSTGIS::readNod
target_link_libraries( osgdb_postgis
	${OPENSCENEGRAPH_LIBRARIES}  
    ${PostgreSQL_LIBRARY}
    ${LWGEOM_LIBRARY}
    ${OPENGL_glu_LIBRARY}
    ${OPENGL_gl_LIBRARY}
    ${GDAL_LIBRARY}
    ${LibPQ_LIBRARY}
    poly2tri
)

add_executable( SFosg_test
    SFosg_test.cpp
    SFosg.cpp
)
set_target_properties( SFosg_test PROPERTIES DEBUG_POSTFIX "d" )
target_link_libraries( SFosg_test
    ${LWGEOM_LIBRARY}
	${OPENSCENEGRAPH_LIBRARIES}  
    ${OPENGL_glu_LIBRARY}
    ${OPENGL_gl_LIBRARY}
    poly2tri
)
add_test(SFosg_test ${EXECUTABLE_OUTPUT_PATH}/SFosg_testd)

add_library( osgdb_mnt MODULE 
    ReaderWriterMNT.cpp 
)
set_target_properties( osgdb_mnt PROPERTIES DEBUG_POSTFIX "d" )
set_target_properties( osgdb_mnt PROPERTIES PREFIX "")
target_link_libraries( osgdb_mnt
	${OPENSCENEGRAPH_LIBRARIES}  
    ${OPENGL_glu_LIBRARY}
    ${OPENGL_gl_LIBRARY}
    ${GDAL_LIBRARY}
)

install( TARGETS  osgdb_postgis osgdb_mnt
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin 
)


