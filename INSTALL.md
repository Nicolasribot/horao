# Ubuntu 12.04

## Packages

Using Ubuntu GIS unstable :

    sudo apt-get install python-software-properties
    sudo add-apt-repository ppa:ubuntugis/ubuntugis-unstable
    sudo apt-get update

Required development headers :

    sudo apt-get install build-essential \
                         cmake \
                         python-qt4 \
                         pyqt4-dev-tools \
                         libopenscenegraph-dev \
                         libboost-dev \
                         libproj-dev \
                         libgdal1-dev \
                         postgresql-9.1-postgis \
                         postgresql-server-dev-9.1 \
                         libpq-dev


## CMake and PostgreSQL

Since Debian layout does not seem to match the location expected by *CMake*, we use a
symlink :

    sudo ln -s /usr/include/postgresql/9.1/server /usr/include/postgresql/server

([Source](http://stackoverflow.com/questions/13920383/findpostgresql-cmake-wont-work-on-ubuntu))


## liblwgeom 2.1

If you use an obsolete version of `liblwgeom`, the compilation will fail
against an undefined function `lwgeom_set_handlers`. (added in PostGIS source
code [in 2013](https://github.com/postgis/postgis/pull/6))

In order to install the last version of this library, we use the PostgreSQL
apt repository :

    sudo sh -c 'echo "deb http://apt.postgresql.org/pub/repos/apt/ precise-pgdg main" >> /etc/apt/sources.list'
    wget --quiet -O - http://apt.postgresql.org/pub/repos/apt/ACCC4CF8.asc | sudo apt-key add -
    sudo apt-get update

And force the upgrade of a particular version :

    sudo apt-get install liblwgeom-dev


## Compilation and installation

    cmake .

Once compilation succeeded, symlink to install the QGis plugin and put
`horaoViewerd` among system binaries :

    ln -sf `pwd`/qgis_plugin ~/.qgis2/python/plugins/Canvas3D
    ln -sf `pwd`/bin/horaoViewerd /usr/local/bin/horaoViewerd
