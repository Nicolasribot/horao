# -*- coding: utf-8 -*-
"""
/***************************************************************************
 Canvas3D
                                 A QGIS plugin
 3D canvas
                              -------------------
        begin                : 2013-08-12
        copyright            : (C) 2013 by Oslandia
        email                : infos@oslandia.com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
"""
# Import the PyQt and QGIS libraries
from PyQt4.QtCore import *
from PyQt4.QtGui import *
from qgis.core import *

from viewer_pipe import ViewerPipe

# Initialize Qt resources from file resources.py
import resources_rc
import os.path
import sys
import subprocess
import math

# constants.

qset = QSettings( "oslandia", "horao_qgis_plugin" )

SIMPLEVIEWER_BIN = qset.value( "horaoviewer_path", "horaoViewer" )

# Turn to true to activate draping by the viewer
DEM_VIEWER_DRAPING = qset.value( "dem_viewer_draping", True )

# distance, in meters, between each vector layer
Z_VECTOR_FIGHT_GAP = qset.value( "z_vector_fight", .5 )
# distance, in meters on top of DEM layer
Z_DEM_FIGHT_GAP = qset.value( "z_dem_fight", 1.5 )
# tile size
TILE_SIZE = qset.value( "tile_size", 2000 )

# will create the settings file if not present
qset.setValue( "horaoviewer_path", SIMPLEVIEWER_BIN )

WIN_TITLE = "Canvas3D"

class LayerInfo:
    def __init__( self ):
        self.id = None
        self.visible = False

    def __init__( self, id, visibility ):
        self.id = id
        self.visible = visibility

class Canvas3D:

    def __init__(self, iface):
        # Save reference to the QGIS interface
        self.iface = iface
        # initialize plugin directory
        self.plugin_dir = os.path.dirname(__file__)

        # map of layer object => LayerInfo
        self.layers = {}

        self.vpipe = ViewerPipe()

        self.signalsConnected = False

    def initGui(self):
        # Create action that will start plugin configuration
        self.action = QAction(
            QIcon(":/plugins/canvas3d/icon.png"),
            u"3D canvas", self.iface.mainWindow())
        # connect the action to the run method
        self.action.triggered.connect(self.run)

        # Add toolbar button and menu item
        self.iface.addToolBarIcon(self.action)
        self.iface.addPluginToMenu( WIN_TITLE, self.action)

    def unload(self):
        # Remove the plugin menu item and icon
        self.iface.removePluginMenu( WIN_TITLE, self.action)
        self.iface.removeToolBarIcon(self.action)

    def sendToViewer( self, cmd, args ):
        if self.vpipe:
            r = self.vpipe.evaluate( cmd, args )
            if r[0] == 'broken_pipe':
                # the viewer is not here anymore, ignoring
                return
            if r[0] != 'ok':
                QMessageBox.warning( None, "Communication error", r[1]['msg'] )

    # called when layer's properties has been changed through UI
    def onPropertiesChanged( self, layer ):
        style = {}
        if layer.type() == 0: # vector
            renderer = layer.rendererV2()
            # first symbol
            sym = renderer.symbols()[0]
            # first symbol layer
            symL = sym.symbolLayer( 0 )
            if symL.type() == 0: # point
                style['fill_color_diffuse'] = symL.color().name()
            elif symL.type() == 2: # polygon
                style['fill_color_diffuse'] = symL.color().name()
        elif layer.type() == 1: # raster
            # get 'colorize' option
            f = layer.hueSaturationFilter()
            if f.colorizeOn():
                style['fill_color_diffuse'] = f.colorizeColor().name()

        # send symbology
        if len(style) > 0:
            style['id'] = layer.id()
            self.sendToViewer( 'setSymbology', style )

    def addLayer( self, layer ):
        providerName = layer.dataProvider().name()

        # get position in layer set (z-index)
        visibleLayers = self.iface.mapCanvas().mapRenderer().layerSet()
        z = 0
        for l in visibleLayers:
            if l == layer.id():
                break
            if layer.type() == 0: # vector
                z = z + Z_VECTOR_FIGHT_GAP
            elif layer.type() == 1: # raster
                z = z + Z_DEM_FIGHT_GAP

        elevationFile = None
        if DEM_VIEWER_DRAPING:
            # get raster layer, if any
            registry = QgsMapLayerRegistry.instance()
            for name, l in registry.mapLayers().iteritems():
                if l.type() == 1:
                    provider = l.dataProvider()
                    if provider.name() == 'gdal' and provider.bandCount() == 1 and provider.dataType( 1 ) != QGis.Byte:
                        elevationFile = l.source()
                        break

        ret = None
        # get layer' style
        if layer.type() == 0: # vector

            if providerName == 'postgres':

                # test 2D/3D geometries:
                is3D = False
                provider = layer.dataProvider()
                sys.stderr.write("get features...\n")
                it = provider.getFeatures()
                sys.stderr.write("end of get features...\n")
                feature = it.next()
                if feature:
                    t = feature.geometry().wkbType()
                    if feature.geometry().wkbType() < 0 : #
                        is3D = True
                it.rewind()
                it.close()

                # parse connection string
                connection = {}
                geocolumn = 'geom'
                args = {}
                query = ''
                table = ''

                # connection info followed by table and queries
                # connection info : k='v' (optional ' for integers)
                # queries : table="...." (geocolumn_name) sql=... until end of line
                (connection_str, queries_str) = layer.source().split("table=")

                s = connection_str.split(' ')
                for si in s:
                    ss = si.split('=')
                    if len(ss) > 1:
                        connection[ ss[0] ] = ss[1].strip("'\"")

                (table, q) = queries_str.split('" ')
                # if the table is a query (from DB manager):
                if table[0:2] == '"(':
                    table.strip('"')
                else:
                    table=table+'"'
                (geocolumn,query) = q.split('sql=')
                geocolumn = geocolumn.strip('() ')

                # use geocolumn only for polygons
                if layer.geometryType() == 2:
                    args['geocolumn'] = geocolumn

                args['id'] = layer.id()
                args['conn_info'] = ' '.join( ["%s='%s'" % (k,v) for k,v in connection.iteritems() if k in ['dbname','user','port']] )
                extent = layer.extent()

                center = self.fullExtent.center()
                args['extent'] = "%f %f,%f %f" % ( extent.xMinimum(), extent.yMinimum(), extent.xMaximum(), extent.yMaximum() )
                args['origin'] = "%f %f %f" % (center.x(), center.y(), z)
                
                if table[0:2] == '"(':
                    # table == query (DB manager)
                    query = table.strip('"()')
                else:
                    query = "SELECT * FROM %s /**WHERE TILE && %s*/" % (table, geocolumn)


                if layer.hasScaleBasedVisibility():
                    lmin = layer.minimumScale()
                    lmax = layer.maximumScale()
                else:
                    # by default, we enable on demand loading
                    lmin = 1
                    lmax = 10000000

                # conversion from 1:N scale to distance from ground
                #
                cnv = self.iface.mapCanvas()
                # FIXME: we use the default FOV here
                fov = 29.1 * math.pi / 180.0
                altMax = 0.5 * (cnv.extent().height() / cnv.scale() * lmax ) / math.tan(fov/2.0)
                altMin = 0.5 * (cnv.extent().height() / cnv.scale() * lmin ) / math.tan(fov/2.0)
                args['lod'] = "%f %f" % (altMax, altMin)
                args['query_0'] = query
                args['tile_size'] = TILE_SIZE
                if elevationFile and not is3D and layer.geometryType() == 2:
                    args['elevation'] = elevationFile
                    
                self.sendToViewer( 'loadVectorPostgis', args )
                self.layers[ layer ] = LayerInfo( layer.id(), False )
                    
        #
        # raster layers
        #
        elif layer.type() == 1:
            provider = layer.dataProvider()

            if provider.name() == 'gdal':
                # warning: band # start at 1
                extent = self.fullExtent
                extent = "%f %f,%f %f" % ( extent.xMinimum(), extent.yMinimum(), extent.xMaximum(), extent.yMaximum() )
                center = self.fullExtent.center()
                origin = "%f %f %f" % ( center.x(), center.y(), z)

                fileSrc = layer.source()

                # look for a .ive file and use it if present
                fileName, fileExt = os.path.splitext( fileSrc )
                iveFile = fileName + ".ive"
                if os.path.exists( iveFile ):
                    fileSrc = iveFile

                if provider.bandCount() == 1 and provider.dataType( 1 ) != QGis.Byte:

                    if layer.hasScaleBasedVisibility():
                        lmin = layer.minimumScale()
                        lmax = layer.maximumScale()
                    else:
                        # by default, we enable on demand loading
                        lmin = 1
                        lmax = 10000000

                    cnv = self.iface.mapCanvas()
                    # FIXME: we use the default FOV here
                    fov = 29.1 * math.pi / 180.0
                    altMax = 0.5 * (cnv.extent().height() / cnv.scale() * lmax ) / math.tan(fov/2.0)
                    altMin = 0.5 * (cnv.extent().height() / cnv.scale() * lmin ) / math.tan(fov/2.0)

                    self.sendToViewer( 'loadElevation', { 'id': layer.id(),
                                                          'file': fileSrc,
                                                          'extent' : extent,
                                                          'origin' : origin,
                                                          'mesh_size_0' : 10, # ???
                                                          'lod' : "%f %f" % (altMax, altMin),
                                                          'tile_size' : TILE_SIZE
                                                          } )
                else:
                    pass
                    # not supported yet self.sendToViewer( 'loadImageGDAL', { 'id': layer.id(), 'url':layer.source()} )

                self.layers[ layer ] = LayerInfo( layer.id(), False )

        # update symbology
        self.onPropertiesChanged( layer )
        # connect to rendererChanged
        QObject.connect( layer, SIGNAL( "rendererChanged()" ), lambda l=layer: self.onPropertiesChanged(l) )

    def removeLayer( self, layer ):
        if self.layers.has_key( layer ):
            layerId = self.layers[ layer ].id
            self.sendToViewer( 'unloadLayer', { 'id': layerId } )
            # disconnect
            QObject.disconnect( layer, SIGNAL( "rendererChanged()" ), lambda l=layer: self.onPropertiesChanged(l) )

            del self.layers[ layer ]

    def setExtent( self, epsg, xmin, ymin, xmax, ymax ):
        center = self.fullExtent.center()
        self.sendToViewer( 'addPlane', { 'id' : 'p0',
                                         'extent' : "%f %f,%f %f" % (xmin, ymin, xmax, ymax),
                                         'origin' : "%f %f 1" % (center.x(), center.y()) } )
        self.sendToViewer( 'setSymbology', { 'id' : 'p0', 
                                             'fill_color_diffuse': '#ffffffff' } )

    def setLayerVisibility( self, layer, visibility ):
        if not self.layers.has_key( layer ):
            return
        layerId = self.layers[ layer ].id
        if visibility:
            self.sendToViewer( 'showLayer', {'id' : layerId } )
        else:
            self.sendToViewer( 'hideLayer', {'id' : layerId } )
        self.layers[ layer ].visible = visibility

    # qgis signal : layer added
    def onLayerAdded( self, layer ):
        self.addLayer( layer )
        self.setLayerVisibility( layer, True )

    # qgis signal : layer removed
    def onLayerRemoved( self, layerId ):
        registry = QgsMapLayerRegistry.instance()
        layer = registry.mapLayer( layerId )
        if layer:
            self.removeLayer( layer )

    # qgis signal : layers changed
    def onLayersChanged( self ):
        renderer = self.iface.mapCanvas().mapRenderer()
        # returns visible layers
        layers = renderer.layerSet()
        for layer, p in self.layers.iteritems():
            if layer.id() in layers and not p.visible:
                self.setLayerVisibility( layer, True )
            if layer.id() not in layers and p.visible:
                self.setLayerVisibility( layer, False )

    # qgis signal : extents changed
    def updateCamera( self ):
        vExtent = self.iface.mapCanvas().extent()
        center = self.fullExtent.center()
        self.sendToViewer( 'lookAt', { 'origin' : "%f %f 0" % (center.x(), center.y()),
                                       'extent' : "%f %f,%f %f" % (vExtent.xMinimum(),
                                                                   vExtent.yMinimum(),
                                                                   vExtent.xMaximum(),
                                                                   vExtent.yMaximum() ) } )


    # run method that performs all the real work
    def run(self):

        registry = QgsMapLayerRegistry.instance()
        if not self.signalsConnected:
            # when a layer is added
            QObject.connect( registry, SIGNAL( "layerWasAdded( QgsMapLayer * )" ), self.onLayerAdded )
            # when a layer is removed
            QObject.connect( registry, SIGNAL( "layerWillBeRemoved( QString )" ), self.onLayerRemoved )
            # when a layer' state changes (in particular: its visibility)
            QObject.connect( self.iface.mapCanvas(), SIGNAL( "layersChanged()" ), self.onLayersChanged )
            # when the extent has changed
            QObject.connect( self.iface.mapCanvas(), SIGNAL( "extentsChanged()" ), self.updateCamera )

            self.signalsConnected = True

        # get the current global extent
        renderer = self.iface.mapCanvas().mapRenderer()
        # store it (will be used as origin)
        self.fullExtent = renderer.fullExtent()
        extent = self.fullExtent
        epsg = renderer.destinationCrs().authid()

        if extent.isEmpty():
            QMessageBox.information( None, WIN_TITLE, "No layer loaded, no extent defined, aborting" )
            return

        try:
            self.vpipe.start( SIMPLEVIEWER_BIN )
        except OSError as e:
            QMessageBox.warning( None, WIN_TITLE, "Problem starting %s: %s" % (SIMPLEVIEWER_BIN, e.strerror ) )
            return

        self.setExtent( epsg, extent.xMinimum(), extent.yMinimum(), extent.xMaximum(), extent.yMaximum() )

        # set camera
        self.updateCamera()

        # load every visible layer
        layers = registry.mapLayers()

        # returns visible layers
        visibleLayers = self.iface.mapCanvas().mapRenderer().layerSet()

        for lid, l in layers.iteritems():
            self.addLayer( l )
            if l.id() in visibleLayers:
                self.setLayerVisibility( l, True )
            else:
                self.setLayerVisibility( l, False )



