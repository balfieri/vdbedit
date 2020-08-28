// Copyright (c) 2020 Robert A. Alfieri
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// vdbedit.cpp - program for making edits to OpenVDB files
// 
#include <openvdb/openvdb.h>
#include <openvdb/tools/Prune.h>
#include <tiffio.h>
#include <iostream>
#include <string>
#include <stdio.h>

using Vec3f = openvdb::Vec3f;
using Vec3d = openvdb::Vec3d;
using Coord = openvdb::Coord;
using CoordBBox = openvdb::math::CoordBBox;

void die( std::string msg ) { std::cout << "ERROR: " << msg << "\n"; exit( 1 ); }

int main( int argc, const char * argv[] )
{
    //------------------------------------------------
    // Process Args
    //------------------------------------------------
    std::string in_name = "";
    std::string tiff_name = "";
    Vec3f       tiff_spacing( 1, 1, 1 );
    std::string out_name = "";
    bool        have_rgb = false;
    Vec3f       rgb;
    bool        have_g = false;
    float       g;
    bool        have_mfpl = false;
    float       mfpl;
    bool        do_prune = false;
    bool        debug = false;
    for( int i = 1; i < argc; i++ )
    {
        std::string arg = argv[i];    
        if ( arg == "-i" ) {
            in_name = argv[++i];
        } else if ( arg == "-o" ) {
            out_name = argv[++i];
        } else if ( arg == "-tiff" ) {
            tiff_name = argv[++i];
        } else if ( arg == "-tiff_spacing" ) {
            float w = std::atof( argv[++i] );
            float h = std::atof( argv[++i] );
            float d = std::atof( argv[++i] );
            tiff_spacing = Vec3f( w, h, d );
        } else if ( arg == "-rgb" ) {
            float r = std::atof( argv[++i] );
            float g = std::atof( argv[++i] );
            float b = std::atof( argv[++i] );
            have_rgb = true;
            rgb = Vec3f( r, g, b );
            std::cout << "rgb=" << r << "," << g << "," << b << "\n";
        } else if ( arg == "-g" ) {
            have_g = true;
            g = std::atof( argv[++i] );
            std::cout << "g=" << g << "\n";
        } else if ( arg == "-mfpl" ) {
            have_mfpl = true;
            mfpl = std::atof( argv[++i] );
            std::cout << "mfpl=" << mfpl << "\n";
        } else if ( arg == "-do_prune" ) {
            do_prune = std::atoi( argv[++i] );
        } else if ( arg == "-d" ) {
            debug = std::atoi( argv[++i] );
        } else {
            die( "unknown option: " + arg );
        }
    }

    openvdb::initialize();

    openvdb::io::File * in_file = nullptr;
    openvdb::GridPtrVecPtr grids;
    TIFF * tiff = nullptr;
    if ( in_name != "" ) {
        //------------------------------------------------
        // Read In Existing Grids
        //------------------------------------------------
        std::cout << "Reading " << in_name << "...\n";
        in_file = new openvdb::io::File( in_name );
        in_file->open();
        grids = in_file->getGrids();
        std::cout << "Found " << grids->size() << " grids in the volume\n";

    } else if ( tiff_name != "" ) {
        //------------------------------------------------
        // Read TIF Stack Into RGB Grid
        //------------------------------------------------
        openvdb::Vec3SGrid::Ptr rgb_grid = openvdb::Vec3SGrid::create();
        openvdb::Vec3SGrid::Accessor rgb_acc = rgb_grid->getAccessor();
        rgb_grid->setGridClass( openvdb::GRID_UNKNOWN );
        rgb_grid->setName( "rgb" );
        rgb_grid->setTransform( openvdb::math::Transform::createLinearTransform( tiff_spacing[0] ) ); // TODO: temporary
        grids->push_back( rgb_grid );

        std::cout << "Reading " << tiff_name << " and converting to a volume...\n";
        tiff = TIFFOpen( tiff_name.c_str(), "r" );
        if ( debug ) TIFFPrintDirectory( tiff, stdout, 0 );
        uint32_t w;
        uint32_t h;
        uint32_t z;
        for( z = 0; ; z++ )
        {
            if ( !TIFFReadDirectory( tiff ) ) break;

            TIFFGetField( tiff, TIFFTAG_IMAGEWIDTH,  &w );
            TIFFGetField( tiff, TIFFTAG_IMAGELENGTH, &h );
            uint32_t pixel_cnt = w*h;
            uint32_t *image = (uint32_t*)_TIFFmalloc( pixel_cnt * sizeof(uint32_t) );
            if ( !TIFFReadRGBAImage( tiff, w, h, image, 0 ) ) die( "unable to read tiff image" + std::to_string(z) + " data" );
            uint32_t *pixel = image;
            for( uint32_t y = 0; y < h; y++ )
            {
                for( uint32_t x = 0; x < w; x++, pixel++ )
                {
                    uint32_t argb = *pixel;
                    uint32_t a = (argb >> 24) & 0xff;
                    uint32_t r = (argb >> 16) & 0xff;
                    uint32_t g = (argb >>  8) & 0xff;
                    uint32_t b = (argb >>  0) & 0xff;
                    if ( a != 0xff ) die( "alpha channel should be 0xff for now" );
                    float r_f = float(r) / float(0xff);
                    float g_f = float(g) / float(0xff);
                    float b_f = float(b) / float(0xff);
                    rgb_acc.setValue( Coord( x, y, z ), Vec3f( r_f, g_f, b_f ) );
                }
            }
            _TIFFfree( image );
        }
        TIFFClose( tiff );
        std::cout << z << " tiff images read\n";

    } else { 
        die( "must provide -i or -tiff options for now" );
    }

    //------------------------------------------------
    // Find widest dimensions from all grids.
    // The 4x4 index-to-world transform must be the same for all grids.
    //------------------------------------------------
    int x_min =  0x7fffffff;
    int x_max = -0x7fffffff;
    int y_min =  0x7fffffff;
    int y_max = -0x7fffffff;
    int z_min =  0x7fffffff;
    int z_max = -0x7fffffff;
    CoordBBox bbox;
    openvdb::math::Transform::Ptr transform_ptr;
    for( int i = 0; i < grids->size(); i++ )
    {
        assert( (*grids)[i]->transform().voxelSize() == voxel_size3 );
        bbox = (*grids)[i]->evalActiveVoxelBoundingBox();
        auto min = bbox.min();
        auto max = bbox.max();
        if (min.x() < x_min) x_min = min.x();
        if (max.x() > x_max) x_max = max.x();
        if (min.y() < y_min) y_min = min.y();
        if (max.y() > y_max) y_max = max.y();
        if (min.z() < z_min) z_min = min.z();
        if (max.z() > z_max) z_max = max.z();
        auto voxel_size = (*grids)[i]->voxelSize();
        bool has_uniform = (*grids)[i]->hasUniformVoxels();
        transform_ptr = (*grids)[i]->transformPtr();
        std::cout << "Volume dimensions after grid_i=" << i << " with ibox=" << bbox << " transform=" << *transform_ptr << 
                     " are: [" << x_min << "," << y_min << "," << z_min << "] .. [" << x_max << "," << y_max << "," << z_max << "]\n";
    }
    bbox = CoordBBox( x_min, y_min, z_min, x_max, y_max, z_max ); // update

    //------------------------------------------------
    // Do Edits
    //------------------------------------------------
    if ( have_rgb ) {
        std::cout << "Filling RGB grid with rgb=" << rgb << "\n";

        //------------------------------------------------
        // Create RGB Grid
        //------------------------------------------------
        openvdb::Vec3SGrid::Ptr rgb_grid     = openvdb::Vec3SGrid::create();
        openvdb::Vec3SGrid::Accessor rgb_acc = rgb_grid->getAccessor();
        rgb_grid->setGridClass( openvdb::GRID_UNKNOWN );
        rgb_grid->setName( "rgb" );
        rgb_grid->setTransform( transform_ptr );
        grids->push_back( rgb_grid );

        rgb_grid->fill( bbox, rgb, true );
    }

    if ( have_g ) {
        std::cout << "Filling G grid with single value g=" << g << "...\n";

        //------------------------------------------------
        // Create G Grid
        //------------------------------------------------
        openvdb::FloatGrid::Ptr g_grid     = openvdb::FloatGrid::create();
        openvdb::FloatGrid::Accessor G_acc = g_grid->getAccessor();
        g_grid->setGridClass( openvdb::GRID_UNKNOWN );
        g_grid->setName( "g" );
        g_grid->setTransform( transform_ptr );
        grids->push_back( g_grid );

        g_grid->fill( bbox, g, true );
    }

    if ( have_mfpl ) {
        std::cout << "Filling MFPL grid with single value mfpl=" << mfpl << "...\n";

        //------------------------------------------------
        // Create MFPL Grid
        //------------------------------------------------
        openvdb::FloatGrid::Ptr mfpl_grid  = openvdb::FloatGrid::create();
        openvdb::FloatGrid::Accessor G_acc = mfpl_grid->getAccessor();
        mfpl_grid->setGridClass( openvdb::GRID_UNKNOWN );
        mfpl_grid->setName( "mfpl" );
        mfpl_grid->setTransform( transform_ptr );
        grids->push_back( mfpl_grid );

        mfpl_grid->fill( bbox, mfpl, true );
    }

    //------------------------------------------------
    // Prune grids.
    // This should not be needed given the use of fill().
    //------------------------------------------------
    if ( do_prune ) {
        for( int i = 0; i < grids->size(); i++ )
        {
            (*grids)[i]->pruneGrid();
        }
    }

    //------------------------------------------------
    // Write output file
    //------------------------------------------------
    if ( out_name != "" ) {
        std::cout << "Writing " << out_name << "...\n";
        openvdb::io::File out_file( out_name );
        out_file.write( *grids );
        out_file.close();
    }

    in_file->close(); // here to avoid freeing grids
}
