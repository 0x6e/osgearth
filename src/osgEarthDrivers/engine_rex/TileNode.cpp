/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2014 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "TileNode"
#include "SurfaceNode"
#include "ProxySurfaceNode"
#include "EngineContext"
#include "Loader"
#include "LoadTileData"
#include "ExpireTiles"
#include "SelectionInfo"
#include "ElevationTextureUtils"

#include <osgEarth/CullingUtils>
#include <osgEarth/ImageUtils>

#include <osg/Uniform>
#include <osg/ComputeBoundsVisitor>

using namespace osgEarth::Drivers::RexTerrainEngine;
using namespace osgEarth;

#define OSGEARTH_TILE_NODE_PROXY_GEOMETRY_DEBUG 0

#define USE_PROXY_SURFACE 0

#define LC "[TileNode] "

namespace
{
    // Scale and bias matrices, one for each TileKey quadrant.
    const osg::Matrixf scaleBias[4] =
    {
        osg::Matrixf(0.5f,0,0,0, 0,0.5f,0,0, 0,0,1.0f,0, 0.0f,0.5f,0,1.0f),
        osg::Matrixf(0.5f,0,0,0, 0,0.5f,0,0, 0,0,1.0f,0, 0.5f,0.5f,0,1.0f),
        osg::Matrixf(0.5f,0,0,0, 0,0.5f,0,0, 0,0,1.0f,0, 0.0f,0.0f,0,1.0f),
        osg::Matrixf(0.5f,0,0,0, 0,0.5f,0,0, 0,0,1.0f,0, 0.5f,0.0f,0,1.0f)
    };
    //const osg::Vec3f scaleBias[4] =
    //{
    //    osg::Vec3f(0.5f, 0.0f, 0.5f),
    //    osg::Vec3f(0.5f, 0.5f, 0.5f),
    //    osg::Vec3f(0.5f, 0.0f, 0.0f),
    //    osg::Vec3f(0.5f, 0.5f, 0.0f)
    //};
}

TileNode::TileNode() : 
_dirty      ( false )
{
    osg::StateSet* stateSet = getOrCreateStateSet();

    // The StateSet must have a dynamic data variance since we plan to alter it
    // as new data becomes available.
    stateSet->setDataVariance(osg::Object::DYNAMIC);

    _count = 0;
}

void
TileNode::create(const TileKey& key, EngineContext* context)
{
    _key = key;

    // Get a shared geometry from the pool that corresponds to this tile key:
    osg::ref_ptr<osg::Geometry> geom;
    context->getGeometryPool()->getPooledGeometry(key, context->getMapFrame().getMapInfo(), geom);

    // Create the drawable for the terrain surface:
    TileDrawable* surfaceDrawable = new TileDrawable(
        key, 
        context->getRenderBindings(), 
        geom.get(),
        context->getOptions().tileSize().get() );

    surfaceDrawable->setDrawAsPatches(false);

    // Create the node to house the tile drawable:
    _surface = new SurfaceNode(
        key,
        context->getMapFrame().getMapInfo(),
        context->getRenderBindings(),
        surfaceDrawable );
    
    // Slot it into the proper render bin:
    osg::StateSet* surfaceSS = _surface->getOrCreateStateSet();
    surfaceSS->setRenderBinDetails(0, "oe.SurfaceBin");
    surfaceSS->setNestRenderBins(false);

    // Create a drawable for land cover geometry.
    // Land cover will be rendered as patch data instead of triangles.
    TileDrawable* patchDrawable = new TileDrawable(
        key, 
        context->getRenderBindings(),
        geom.get(),
        context->getOptions().tileSize().get() );

    patchDrawable->setDrawAsPatches(true);

    // And a node to house that as well:
    _landCover = new SurfaceNode(
        key,
        context->getMapFrame().getMapInfo(),
        context->getRenderBindings(),
        patchDrawable );

    // add a cluster-culling callback:
    if ( context->getMapFrame().getMapInfo().isGeocentric() )
    {
        addCullCallback( ClusterCullingFactory::create(key.getExtent()) );
    }

    // PPP: Better way to do this rather than here?
    // Can't do it at RexTerrainEngineNode level, because the SurfaceNode is not valid yet
    if (context->getSelectionInfo().initialized()==false)
    {
        SelectionInfo& selectionInfo = const_cast<SelectionInfo&>(context->getSelectionInfo());

        unsigned uiFirstLOD = *(context->_options.firstLOD());
        unsigned uiMaxLod   = *(context->_options.maxLOD());
        unsigned uiTileSize = *(context->_options.tileSize());

        selectionInfo.initialize(uiFirstLOD, uiMaxLod, uiTileSize, getVisibilityRangeHint(uiFirstLOD));
    }

    // initialize all the per-tile uniforms the shaders will need:
    createTileUniforms();
    updateTileUniforms(context->getSelectionInfo());

    // Set up a data container for multipass layer rendering:
    _mptex = new MPTexture();
    surfaceDrawable->setMPTexture( _mptex.get() );

    // need to recompute the bounds after adding payload:
    dirtyBound();

    // signal the tile to start loading data:
    setDirty( true );

    // register me.
    context->liveTiles()->add( this );
}

osg::BoundingSphere
TileNode::computeBound() const
{
    return _surface.valid() ? _surface->computeBound() : osg::BoundingSphere();
}

bool
TileNode::isDormant(osg::NodeVisitor& nv) const
{
    return
        nv.getFrameStamp() &&
        nv.getFrameStamp()->getFrameNumber() - _lastTraversalFrame > 2u;
}

void
TileNode::setElevationRaster(const osg::Image* image, const osg::Matrixf& matrix)
{
    if ( _surface.valid() )
        _surface->setElevationRaster( image, matrix );

    if ( _landCover.valid() )
        _landCover->setElevationRaster( image, matrix );
}

const osg::Image*
TileNode::getElevationRaster() const
{
    return _surface->getElevationRaster();
}

const osg::Matrixf&
TileNode::getElevationMatrix() const
{
    return _surface->getElevationMatrix();
}

void
TileNode::createTileUniforms()
{
    // Install the tile key uniform.
    _tileKeyUniform = new osg::Uniform("oe_tile_key", osg::Vec4f(0,0,0,0));
    getStateSet()->addUniform( _tileKeyUniform.get() );

    _tileMorphUniform = new osg::Uniform("oe_tile_morph_constants", osg::Vec4f(0,0,0,0));
    getStateSet()->addUniform( _tileMorphUniform.get() );

    _tileGridDimsUniform = new osg::Uniform("oe_tile_grid_dimensions", osg::Vec4f(0,0,0,0));
    getStateSet()->addUniform( _tileGridDimsUniform.get() );

    _tileExtentsUniform = new osg::Uniform("oe_tile_extents", osg::Vec4f(0,0,0,0));
    getStateSet()->addUniform( _tileExtentsUniform.get() );

    _tileCenterToCameraDistUniform = new osg::Uniform("oe_tile_camera_to_tilecenter", osg::Vec4f(0,0,0,0));
    getStateSet()->addUniform( _tileCenterToCameraDistUniform.get() );
}

void
TileNode::updateTileUniforms(const SelectionInfo& selectionInfo)
{
    //assert(_surface.valid());
    // update the tile key uniform
    const osg::BoundingBox& bbox = _surface->getAlignedBoundingBox();
    float width = std::max( (bbox.xMax()-bbox.xMin()), (bbox.yMax()-bbox.yMin()) );

    unsigned tw, th;
    _key.getProfile()->getNumTiles(_key.getLOD(), tw, th);

    _tileKeyUniform->set(osg::Vec4f(_key.getTileX(), th-_key.getTileY()-1.0f, _key.getLOD(), width));

    // update the morph constants

    float fStart = (float)selectionInfo.visParameters(_key.getLOD())._fMorphStart;
    float fEnd   = (float)selectionInfo.visParameters(_key.getLOD())._fMorphEnd;

    float one_by_end_minus_start = fEnd - fStart;
    one_by_end_minus_start = 1.0f/one_by_end_minus_start;

    osg::Vec4f morphConstants(
          fStart
        , one_by_end_minus_start
        , fEnd * one_by_end_minus_start
        , one_by_end_minus_start
        );

    _tileMorphUniform->set( morphConstants );

    // Update grid dims
    float fGridDims = selectionInfo.gridDimX()-1;
    _tileGridDimsUniform->set(osg::Vec4f(
        fGridDims,
        fGridDims*0.5f,
        2.0/fGridDims,
        selectionInfo.lodForMorphing(_key.getProfile()->getSRS()->isProjected())));

    // update tile extents
    float fXExtents = fabs(bbox.xMax()-bbox.xMin());
    float fYExtents = fabs(bbox.yMax()-bbox.yMin());
    _tileExtentsUniform->set(osg::Vec4f(fXExtents,fYExtents,0,0));

    const osg::Image* er = getElevationRaster();
    if ( er )
    {
        getOrCreateStateSet()->getOrCreateUniform("oe_tile_elevationSize", osg::Uniform::FLOAT)->set( (float)er->s() );
    }
}

void
TileNode::updatePerFrameUniforms(const osg::NodeVisitor& nv)
{
    if (_tileCenterToCameraDistUniform.get())
    {
        osg::Vec4f vPerFrameUniform(getTileCenterToCameraDistance(nv),0,0,0);
        _tileCenterToCameraDistUniform->set((vPerFrameUniform));
    }
}

bool
TileNode::isReadyToTraverse() const
{
    // Later, we might replace this if the tile isn't immediately created at cull time.
    return true;
}

void
TileNode::setDirty(bool value)
{
    _dirty = value;
}

void
TileNode::releaseGLObjects(osg::State* state) const
{
    if ( getStateSet() )
        getStateSet()->releaseGLObjects(state);
    if ( _surface.valid() )
        _surface->releaseGLObjects(state);
    if ( _landCover.valid() )
        _landCover->releaseGLObjects(state);

    osg::Group::releaseGLObjects(state);
}

float
TileNode::getTileCenterToCameraDistance(const osg::NodeVisitor& nv) const
{
    const osg::Vec3& vTileCenter = getBound().center();
    return nv.getDistanceToViewPoint( vTileCenter, true );
}

float
TileNode::getVisibilityRangeHint(unsigned firstLOD) const
{
    
    if (getTileKey().getLOD()!=firstLOD)
    {
        OE_INFO << LC <<"Error: Visibility Range hint can be computed only using the first LOD"<<std::endl;
        return -1;
    }
    // The motivation here is to use the extents of the tile at lowest resolution
    // along with a factor as an estimate of the visibility range
    const float factor = 6.0f * 2.5f;
    const osg::BoundingBox& box = _surface->getAlignedBoundingBox();
    return factor * 0.5*std::max( box.xMax()-box.xMin(), box.yMax()-box.yMin() );
}

// 0=off, 1=on
#define OSGEARTH_REX_TILE_NODE_DEBUG_TRAVERSAL 0

bool
TileNode::shouldSubDivide(osg::NodeVisitor& nv, const SelectionInfo& selectionInfo, float zoomFactor)
{
    unsigned currLOD = _key.getLOD();
    if (   currLOD <  selectionInfo.numLods()
        && currLOD != selectionInfo.numLods()-1)
    {
        osg::Vec3 cameraPos = nv.getViewPoint();

#if OSGEARTH_REX_TILE_NODE_DEBUG_TRAVERSAL
        OE_INFO << LC <<cameraPos.x()<<" "<<cameraPos.y()<<" "<<cameraPos.z()<<" "<<std::endl;
        OE_INFO << LC <<"LOD Scale: "<<fZoomFactor<<std::endl;
#endif  
        float radius = (float)selectionInfo.visParameters(currLOD+1)._fVisibility;
        bool anyChildVisible = _surface->anyChildBoxIntersectsSphere(cameraPos, radius*radius, zoomFactor);
        return anyChildVisible;
    }
    return false;
}

void TileNode::cull(osg::NodeVisitor& nv)
{
    if ( nv.getFrameStamp() )
    {
        _lastTraversalFrame.exchange( nv.getFrameStamp()->getFrameNumber() );
    }

    unsigned currLOD = getTileKey().getLOD();

    // TODO: rework this to make it multi-camera safe
    updatePerFrameUniforms(nv);

#if OSGEARTH_REX_TILE_NODE_DEBUG_TRAVERSAL
    if (currLOD==0)
    {
        OE_INFO << LC <<"Traversing: "<<"\n";    
    }
#endif
    osgUtil::CullVisitor* cull = dynamic_cast<osgUtil::CullVisitor*>( &nv );

    EngineContext* context = static_cast<EngineContext*>( nv.getUserData() );
    const SelectionInfo& selectionInfo = context->getSelectionInfo();

    const double maxCullTime = 4.0/1000.0;

    bool subdivide = shouldSubDivide(nv, selectionInfo, cull->getLODScale()); // && context->getElapsedCullTime() < maxCullTime;

    // If *any* of the children are visible, subdivide.
    if (subdivide)
    {
        // We are in range of the child nodes. Either draw them or load them.
        unsigned numChildrenReady = 0;

        // If the children don't exist, create them and inherit the parent's data.
        if ( getNumChildren() == 0 )
        {
            Threading::ScopedMutexLock exclusive(_mutex);
            if ( getNumChildren() == 0 )
            {
                createChildren( context );
            }
        }

        // All 4 children must be ready before we can traverse any of them:
        for(unsigned i = 0; i < 4; ++i)
        {                
            if ( getSubTile(i)->isReadyToTraverse() )
            {
                ++numChildrenReady;
            }
        }

        // If all are ready, traverse them now.
        if ( numChildrenReady == 4 )
        {
            // TODO:
            // If we do thing, we need to quite sure that all 4 children will be accepted into
            // the draw set. Perhaps isReadyToTraverse() needs to check that.
            _children[0]->accept( nv );
            _children[1]->accept( nv );
            _children[2]->accept( nv );
            _children[3]->accept( nv );
        }

        // Not all children are ready, so cull the current payload.
        else if ( _surface.valid() )
        {
            _surface->accept( nv );
        }
    }

    // If children are outside camera range, draw the payload and expire the children.
    else if ( _surface.valid() )
    {
        _surface->accept( nv );

        if ( getNumChildren() > 0 && context->maxLiveTilesExceeded() )
        {
            if (getSubTile(0)->isDormant( nv ) &&
                getSubTile(1)->isDormant( nv ) &&
                getSubTile(2)->isDormant( nv ) &&
                getSubTile(3)->isDormant( nv ))
            {
                expireChildren( nv );
            }
        }
    }

    // Traverse land cover bins at this LOD.
    for(int i=0; i<context->landCoverBins()->size(); ++i)
    {
        const LandCoverBin& bin = context->landCoverBins()->at(i);
        if ( bin._lod == getTileKey().getLOD() )
        {
            cull->pushStateSet( bin._stateSet.get() );
            _landCover->accept( nv );
            cull->popStateSet();
        }
    }

    // If this tile is marked dirty, try loading data.
    if ( _dirty )
    {
        load( nv );
    }
}

void
TileNode::traverse(osg::NodeVisitor& nv)
{
    // Cull only:
    if ( nv.getVisitorType() == nv.CULL_VISITOR )
    {
        cull(nv);
    }

    // Everything else: update, GL compile, intersection, compute bound, etc.
    else
    {
        // If there are child nodes, traverse them:
        int numChildren = getNumChildren();
        if ( numChildren > 0 )
        {
            for(int i=0; i<numChildren; ++i)
            {
                _children[i]->accept( nv );
            }
        }

        // Otherwise traverse the surface.
        // TODO: in what situations should we traverse the landcover as well? GL compile?
        else 
        {
            _surface->accept( nv );
        }
    }
}

void
TileNode::createChildren(EngineContext* context)
{
    // Create the four child nodes.
    for(unsigned quadrant=0; quadrant<4; ++quadrant)
    {
        TileNode* node = new TileNode();

        // Build the surface geometry:
        node->create( getTileKey().createChildKey(quadrant), context );

        // Add to the scene graph.
        addChild( node );

        // Inherit the samplers with new scale/bias information.
        node->inheritState( context );
    }
    
    OE_DEBUG << LC << "Creating children of: " << getTileKey().str() << "; count = " << (++_count) << "\n";
}

bool
TileNode::inheritState(EngineContext* context)
{
    // Find the parent node. It will only be null if this is a "first LOD" tile.
    TileNode* parent = getNumParents() > 0 ? dynamic_cast<TileNode*>(getParent(0)) : 0L;

    bool changesMade = false;

    // which quadrant is this tile in?
    unsigned quadrant = getTileKey().getQuadrant();

    // default inheritance of the elevation data for bounding purposes:
    osg::ref_ptr<const osg::Image> elevRaster;
    osg::Matrixf                   elevMatrix;
    if ( parent )
    {
        elevRaster = parent->getElevationRaster();
        elevMatrix = parent->getElevationMatrix();
        elevMatrix.preMult( scaleBias[quadrant] );
    }

    // Find all the sampler matrix uniforms and scale/bias them to the current quadrant.
    // This will inherit textures and use the proper sub-quadrant until new data arrives (later).
    for( RenderBindings::const_iterator binding = context->getRenderBindings().begin(); binding != context->getRenderBindings().end(); ++binding )
    {
        if ( binding->usage().isSetTo(binding->COLOR) )
        {
            if ( parent && parent->getStateSet() )
            {
                MPTexture* parentMPTex = parent->getMPTexture();
                _mptex->inheritState( parentMPTex, scaleBias[quadrant] );
                changesMade = true;
            }
        }

        else if ( binding->usage().isSetTo(binding->COLOR_PARENT) )
        {
            //nop -- this is handled as part of the COLOR binding
        }

        else
        {
            osg::StateAttribute* sa = getStateSet()->getTextureAttribute(binding->unit(), osg::StateAttribute::TEXTURE);        

            // If the attribute isn't present, that means we are inheriting it from above.
            // So construct a new scale/bias matrix.
            if ( sa == 0L )
            {
                osg::Matrixf matrix;

                // Find the parent's matrix and scale/bias it to this quadrant:
                if ( parent && parent->getStateSet() )
                {
                    osg::Uniform* matrixUniform = parent->getStateSet()->getUniform( binding->matrixName() );
                    if ( matrixUniform )
                    {
                        matrixUniform->get( matrix );
                        matrix.preMult( scaleBias[quadrant] );
                    }
                }

                // Add a new uniform with the scale/bias'd matrix:
                getOrCreateStateSet()->addUniform( new osg::Uniform(binding->matrixName().c_str(), matrix) );
                changesMade = true;
            }

            // If this is elevation data, record the new raster so we can apply it to the node.
            else if ( binding->usage().isSetTo(binding->ELEVATION) )
            {
                osg::Texture* t = static_cast<osg::Texture*>(sa);
                elevRaster = t->getImage(0);
                elevMatrix = osg::Matrixf::identity();
            }
        }
    }

    // If we found one, communicate it to the node and its children.
    if (elevRaster.valid())
    {
//        if (elevRaster.get() != getElevationRaster() ||
//            elevScaleBias    != getElevationMatrix() )
        {
            setElevationRaster( elevRaster.get(), elevMatrix );
            changesMade = true;
        }
    }    

#if 0 // debugging
    // validate that the elevation rasters are the same. --debugging--
    const SamplerBinding* eb = SamplerBinding::findUsage(context->getRenderBindings(), SamplerBinding::ELEVATION);
    osg::Uniform* uu = getStateSet()->getUniform(eb->matrixName());
    osg::Matrixf mm;
    uu->get(mm);
    if ( elevScaleBias != mm ) {
        OE_WARN << LC << "key = " << _key.str() << "; matrices don't match\n";
    }
#endif

    // finally, update the uniforms for terrain morphing
    updateTileUniforms( context->getSelectionInfo() );

    if ( !changesMade )
    {
        OE_INFO << LC << _key.str() << ", good, no changes :)\n";
    }
    else
    {
        dirtyBound();
    }

    return changesMade;
}

void
TileNode::mergeStateSet(osg::StateSet* stateSet, MPTexture* mptex, const RenderBindings& bindings)
{
    _mptex->merge( mptex );    
    getStateSet()->merge(*stateSet);
}

void
TileNode::load(osg::NodeVisitor& nv)
{
    // Access the context:
    EngineContext* context = static_cast<EngineContext*>( nv.getUserData() );

    // Create a new load request on demand:
    if ( !_loadRequest.valid() )
    {
        Threading::ScopedMutexLock lock(_mutex);
        if ( !_loadRequest.valid() )
        {
            _loadRequest = new LoadTileData( this, context );
        }
    }

    // Prioritize by LOD.
    float priority = - (float)getTileKey().getLOD();

    if ( context->getOptions().highResolutionFirst() == true )
        priority = -priority;

    // Submit to the loader.
    context->getLoader()->load( _loadRequest.get(), priority, nv );
}

void
TileNode::expireChildren(osg::NodeVisitor& nv)
{
    OE_DEBUG << LC << "Expiring children of " << getTileKey().str() << "\n";

    EngineContext* context = static_cast<EngineContext*>( nv.getUserData() );
    if ( !_expireRequest.valid() )
    {
        Threading::ScopedMutexLock lock(_mutex);
        if ( !_expireRequest.valid() )
        {
            _expireRequest = new ExpireTiles(this, context);
        }
    }
       
    // Low priority for expiry requests.
    const float lowPriority = -100.0f;
    context->getLoader()->load( _expireRequest.get(), lowPriority, nv );
}
