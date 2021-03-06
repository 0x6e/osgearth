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
#include "TileDrawable"
#include "MPTexture"

#include <osg/Version>
#include <osgUtil/MeshOptimizers>
#include <iterator>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/ImageUtils>

using namespace osg;
using namespace osgEarth::Drivers::RexTerrainEngine;
using namespace osgEarth;

#define LC "[TileDrawable] "

TileDrawable::TileDrawable(const TileKey&        key,
                           const RenderBindings& bindings,
                           osg::Geometry*        geometry,
                           int                   tileSize) :
osg::Drawable( ),
_key         ( key ),
_bindings    ( bindings ),
_geom        ( geometry ),
_tileSize    ( tileSize ),
_drawPatch   ( false )
{
    setUseVertexBufferObjects( true );
    setUseDisplayList( false );

    _supportsGLSL = Registry::capabilities().supportsGLSL();

    // establish uniform name IDs.
    _uidUniformNameID             = osg::Uniform::getNameID( "oe_layer_uid" );
    _orderUniformNameID           = osg::Uniform::getNameID( "oe_layer_order" );
    _opacityUniformNameID         = osg::Uniform::getNameID( "oe_layer_opacity" );
    _texMatrixUniformNameID       = osg::Uniform::getNameID( "oe_layer_texMatrix" );
    _texMatrixParentUniformNameID = osg::Uniform::getNameID( "oe_layer_texParentMatrix" );
    _texParentExistsUniformNameID = osg::Uniform::getNameID( "oe_layer_texParentExists" );

    _textureImageUnit       = SamplerBinding::findUsage(bindings, SamplerBinding::COLOR)->unit();
    _textureParentImageUnit = SamplerBinding::findUsage(bindings, SamplerBinding::COLOR_PARENT)->unit();
}

void
TileDrawable::drawPrimitivesImplementation(osg::RenderInfo& renderInfo) const
{
    if ( _drawPatch )
        drawPatches( renderInfo );
    else
        drawSurface( renderInfo );
}


void
TileDrawable::drawPatches(osg::RenderInfo& renderInfo) const
{
    if ( _geom->getNumPrimitiveSets() < 1 )
        return;

    osg::State& state = *renderInfo.getState(); 
    
    const osg::DrawElementsUShort* de = static_cast<osg::DrawElementsUShort*>(_geom->getPrimitiveSet(0));
    osg::GLBufferObject* ebo = de->getOrCreateGLBufferObject(state.getContextID());
    state.bindElementBufferObject(ebo);
    if (ebo)
        glDrawElements(GL_PATCHES, de->size(), GL_UNSIGNED_SHORT, (const GLvoid *)(ebo->getOffset(de->getBufferIndex())));
    else
        glDrawElements(GL_PATCHES, de->size(), GL_UNSIGNED_SHORT, &de->front());
}

struct StateHack : public osg::State {
    void check() const {
        const UniformMap::const_iterator i = _uniformMap.find("oe_tile_elevationTexMatrix");
        if ( i != _uniformMap.end() ) {
            const UniformStack& s = i->second;
            const UniformStack::UniformPair& p = s.uniformVec.back();
            osg::Matrixf mat;
            p.first->get(mat);
            OE_INFO << "scale=" << mat(0,0) << " x " << mat(1,1) << "; u=" << mat(3,0) << ", " << mat(3,1) << "\n";
        }
        else {
            OE_WARN << "ETEXMAT NOT FOUND!\n";
        }
    }
};


void
TileDrawable::drawSurface(osg::RenderInfo& renderInfo) const
{
    unsigned layersDrawn = 0;

    osg::State& state = *renderInfo.getState();    

    // access the GL extensions interface for the current GC:
    const osg::Program::PerContextProgram* pcp = 0L;

#if OSG_MIN_VERSION_REQUIRED(3,3,3)
	osg::ref_ptr<osg::GLExtensions> ext;
#else
    osg::ref_ptr<osg::GL2Extensions> ext;
#endif
    unsigned contextID;

    if (_supportsGLSL)
    {
        contextID = state.getContextID();
#if OSG_MIN_VERSION_REQUIRED(3,3,3)
		ext = osg::GLExtensions::Get(contextID, true);
#else   
		ext = osg::GL2Extensions::Get( contextID, true );
#endif
        pcp = state.getLastAppliedProgramObject();
    }

    // safely latch
    if ( _geom->getNumPrimitiveSets() < 1 )
        return;

    // cannot store these in the object since there could be multiple GCs (and multiple
    // PerContextPrograms) at large
    GLint opacityLocation            = -1;
    GLint uidLocation                = -1;
    GLint orderLocation              = -1;
    GLint texMatrixLocation          = -1;
    GLint texMatrixParentLocation    = -1;
    GLint texParentExistsLocation    = -1;

    // The PCP can change (especially in a VirtualProgram environment). So we do need to
    // requery the uni locations each time unfortunately. TODO: explore optimizations.
    if ( pcp )
    {
        opacityLocation             = pcp->getUniformLocation( _opacityUniformNameID );
        uidLocation                 = pcp->getUniformLocation( _uidUniformNameID );
        orderLocation               = pcp->getUniformLocation( _orderUniformNameID );
        texMatrixLocation           = pcp->getUniformLocation( _texMatrixUniformNameID );
        texMatrixParentLocation     = pcp->getUniformLocation( _texMatrixParentUniformNameID );
        texParentExistsLocation     = pcp->getUniformLocation( _texParentExistsUniformNameID );
    }
    
    //if ( _key.str() == "22/2538301/1110225" )
    //{
    //    const StateHack* hack = reinterpret_cast<const StateHack*>(renderInfo.getState());
    //    hack->check();
    //}

    float prevOpacity = -1.0f;
    if ( _mptex.valid() && !_mptex->getPasses().empty() )
    {
        float prevOpacity = -1.0f;

        // in FFP mode, we need to enable the GL mode for texturing:
        if ( !pcp )
            state.applyMode(GL_TEXTURE_2D, true);

        optional<bool> texParentExists_lastValue;

        for(MPTexture::Passes::const_iterator p = _mptex->getPasses().begin();
            p != _mptex->getPasses().end();
            ++p)
        {
            const MPTexture::Pass& pass = *p;

            if ( pass._layer->getVisible() && pass._layer->getOpacity() > 0.1 )
            {
                // Apply the texture.
                state.setActiveTextureUnit( _textureImageUnit );
                const osg::StateAttribute* lastTex = state.getLastAppliedTextureAttribute(_textureImageUnit, osg::StateAttribute::TEXTURE);
                if ( lastTex != pass._texture.get() )
                    pass._texture->apply( state );

                // Apply the texture matrix.
                ext->glUniformMatrix4fv( texMatrixLocation, 1, GL_FALSE, pass._textureMatrix.ptr() );

                bool texParentExists = pass._parentTexture.valid();
                if ( texParentExists )
                {
                    // Apply the parent texture.
                    state.setActiveTextureUnit( _textureParentImageUnit );
                    const osg::StateAttribute* lastTex = state.getLastAppliedTextureAttribute(_textureParentImageUnit, osg::StateAttribute::TEXTURE);
                    if ( lastTex != pass._parentTexture.get() )
                        pass._parentTexture->apply( state );

                    // Apply the parent texture matrix.
                    ext->glUniformMatrix4fv( texMatrixParentLocation, 1, GL_FALSE, pass._parentTextureMatrix.ptr() );
                }

                if ( !texParentExists_lastValue.isSetTo(texParentExists) )
                {
                    texParentExists_lastValue = texParentExists;
                    ext->glUniform1f( texParentExistsLocation, texParentExists? 1.0f : 0.0f );
                }

                // Order uniform (TODO: evaluate whether we still need this)
                if ( orderLocation >= 0 )
                {
                    ext->glUniform1i( orderLocation, (GLint)layersDrawn );
                }

                // assign the layer UID:
                if ( uidLocation >= 0 )
                {
                    ext->glUniform1i( uidLocation, (GLint)pass._layer->getUID() );
                }

                // apply opacity:
                if ( opacityLocation >= 0 )
                {
                    float opacity = pass._layer->getOpacity();
                    if ( opacity != prevOpacity )
                    {
                        ext->glUniform1f( opacityLocation, (GLfloat)opacity );
                        prevOpacity = opacity;
                    }
                }

                _geom->getPrimitiveSet(0)->draw(state, true);

                ++layersDrawn;
            }
        }
    }

    // No mptex or no layers in the mptex? Draw simple.
    if ( layersDrawn == 0 )
    {
        if ( opacityLocation >= 0 )
            ext->glUniform1f( opacityLocation, (GLfloat)1.0f );

        if ( uidLocation >= 0 )
            ext->glUniform1i( uidLocation, (GLint)-1 );

        if ( orderLocation >= 0 )
            ext->glUniform1i( orderLocation, (GLint)0 );
        
        _geom->getPrimitiveSet(0)->draw(state, true);
    }

}

void
TileDrawable::setElevationRaster(const osg::Image*   image,
                                 const osg::Matrixf& scaleBias)
{
    _elevationRaster = image;
    _elevationScaleBias = scaleBias;

    if (osg::equivalent(0.0f, _elevationScaleBias(0,0)) ||
        osg::equivalent(0.0f, _elevationScaleBias(1,1)))
    {
        OE_WARN << "("<<_key.str()<<") precision error\n";
    }

    dirtyBound();
}

const osg::Image*
TileDrawable::getElevationRaster() const
{
    return _elevationRaster.get();
}

const osg::Matrixf&
TileDrawable::getElevationMatrix() const
{
    return _elevationScaleBias;
}

void
TileDrawable::accept(osg::PrimitiveFunctor& f) const
{
    const osg::Vec3Array* verts   = static_cast<osg::Vec3Array*>(_geom->getVertexArray());
    const osg::Vec3Array* normals = static_cast<osg::Vec3Array*>(_geom->getNormalArray());

    if ( _elevationRaster.valid() )
    {
        ImageUtils::PixelReader elevation(_elevationRaster.get());
        elevation.setBilinear(true);

        float
            scaleU = _elevationScaleBias(0,0),
            scaleV = _elevationScaleBias(1,1),
            biasU  = _elevationScaleBias(3,0),
            biasV  = _elevationScaleBias(3,1);

        //float
        //    texelScale = (float)(_elevationRaster->s()-1)/(float)_elevationRaster->s(),
        //    texelBias  = 0.5f/(float)(_elevationRaster->s());

        if ( osg::equivalent(scaleU, 0.0f) || osg::equivalent(scaleV, 0.0f) )
        {
            OE_WARN << LC << "Precision loss in tile " << _key.str() << "\n";
        }
    
        for(int t=0; t<_tileSize-1; ++t)
        {
            float v0 = (float)t     / (float)(_tileSize-1);
            float v1 = (float)(t+1) / (float)(_tileSize-1);

            v0 = v0*scaleV + biasV;
            v1 = v1*scaleV + biasV;

            f.begin( GL_QUAD_STRIP );

            for(int s=0; s<_tileSize; ++s)
            {
                float u = (float)s / (float)(_tileSize-1);
                
                u = u*scaleU + biasU;

                int index = t*_tileSize + s;
                {
                    const osg::Vec3f& vert   = (*verts)[index];
                    const osg::Vec3f& normal = (*normals)[index];
                    float h = elevation(u, v0).r();
                    f.vertex( vert + normal*h );
                }

                index += _tileSize;
                {
                    const osg::Vec3f& vert = (*verts)[index];
                    const osg::Vec3f& normal = (*normals)[index];
                    float h = elevation(u, v1).r();
                    f.vertex( vert + normal*h );
                }
            }

            f.end();
        }
    }

    // no elevation
    else
    {
        for(int t=0; t<_tileSize-1; ++t)
        {
            f.begin( GL_QUAD_STRIP );

            for(int s=0; s<_tileSize; ++s)
            {
                int index = t*_tileSize + s;
                f.vertex( (*verts)[index] );
                f.vertex( (*verts)[index + _tileSize] );
            }

            f.end();
        }
    }
}

void 
TileDrawable::releaseGLObjects(osg::State* state) const
{
    osg::Drawable::releaseGLObjects( state );

    if ( _geom.valid() )
    {
        _geom->releaseGLObjects( state );
    }
}


void
TileDrawable::resizeGLObjectBuffers(unsigned maxSize)
{
    osg::Drawable::resizeGLObjectBuffers( maxSize );

    if ( _geom.valid() )
    {
        _geom->resizeGLObjectBuffers( maxSize );
    }

    if ( _pcd.size() < maxSize )
    {
        _pcd.resize(maxSize);
    }
}


void 
TileDrawable::compileGLObjects(osg::RenderInfo& renderInfo) const
{
    osg::Drawable::compileGLObjects( renderInfo );

    if ( _geom.valid() )
    {
        _geom->compileGLObjects( renderInfo );
    }

    // unbind the BufferObjects
    //extensions->glBindBuffer(GL_ARRAY_BUFFER_ARB,0);
    //extensions->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB,0);
}


#if OSG_VERSION_GREATER_OR_EQUAL(3,1,8)
#   define GET_ARRAY(a) (a)
#else
#   define GET_ARRAY(a) (a).array
#endif


void
TileDrawable::drawImplementation(osg::RenderInfo& renderInfo) const
{
    State& state = *renderInfo.getState();
    bool checkForGLErrors = state.getCheckForGLErrors() == osg::State::ONCE_PER_ATTRIBUTE;
    if ( checkForGLErrors ) state.checkGLErrors("start of TileDrawable::drawImplementation()");

#if OSG_MIN_VERSION_REQUIRED(3,3,1)
    _geom->drawVertexArraysImplementation( renderInfo );
#else
    drawVertexArraysImplementation( renderInfo );
#endif

    drawPrimitivesImplementation( renderInfo );

    if ( checkForGLErrors ) state.checkGLErrors("end of TileDrawable::drawImplementation()");
    
    // unbind the VBO's if any are used.
    state.unbindVertexBufferObject();
    state.unbindElementBufferObject();
}


#if OSG_MIN_VERSION_REQUIRED(3,1,8)
void
TileDrawable::drawVertexArraysImplementation(osg::RenderInfo& renderInfo) const
{
    if ( !_geom.valid() )
        return;

    State& state = *renderInfo.getState();

    bool handleVertexAttributes = !_geom->getVertexAttribArrayList().empty();

    ArrayDispatchers& arrayDispatchers = state.getArrayDispatchers();

    arrayDispatchers.reset();
    arrayDispatchers.setUseVertexAttribAlias(state.getUseVertexAttributeAliasing());

    arrayDispatchers.activateNormalArray(_geom->getNormalArray());
    arrayDispatchers.activateColorArray(_geom->getColorArray());
    arrayDispatchers.activateSecondaryColorArray(_geom->getSecondaryColorArray());
    arrayDispatchers.activateFogCoordArray(_geom->getFogCoordArray());

    if (handleVertexAttributes)
    {
        for(unsigned int unit=0;unit<_geom->getVertexAttribArrayList().size();++unit)
        {
            arrayDispatchers.activateVertexAttribArray(unit, _geom->getVertexAttribArray(unit));
        }
    }

    // dispatch any attributes that are bound overall
    arrayDispatchers.dispatch(osg::Array::BIND_OVERALL,0);

    state.lazyDisablingOfVertexAttributes();

    // set up arrays
    if( _geom->getVertexArray() )
        state.setVertexPointer(_geom->getVertexArray());

    if (_geom->getNormalArray() && _geom->getNormalArray()->getBinding()==osg::Array::BIND_PER_VERTEX)
        state.setNormalPointer(_geom->getNormalArray());

    if (_geom->getColorArray() && _geom->getColorArray()->getBinding()==osg::Array::BIND_PER_VERTEX)
        state.setColorPointer(_geom->getColorArray());

    if (_geom->getSecondaryColorArray() && _geom->getSecondaryColorArray()->getBinding()==osg::Array::BIND_PER_VERTEX)
        state.setSecondaryColorPointer(_geom->getSecondaryColorArray());

    if (_geom->getFogCoordArray() && _geom->getFogCoordArray()->getBinding()==osg::Array::BIND_PER_VERTEX)
        state.setFogCoordPointer(_geom->getFogCoordArray());

    for(unsigned int unit=0;unit<_geom->getTexCoordArrayList().size();++unit)
    {
        const Array* array = _geom->getTexCoordArray(unit);
        if (array)
        {
            state.setTexCoordPointer(unit,array);
        }
    }

    if ( handleVertexAttributes )
    {
        for(unsigned int index = 0; index < _geom->getVertexAttribArrayList().size(); ++index)
        {
            const Array* array = _geom->getVertexAttribArray(index);
            if (array && array->getBinding()==osg::Array::BIND_PER_VERTEX)
            {
                if (array->getPreserveDataType())
                {
                    GLenum dataType = array->getDataType();
                    if (dataType==GL_FLOAT) state.setVertexAttribPointer( index, array );
                    else if (dataType==GL_DOUBLE) state.setVertexAttribLPointer( index, array );
                    else state.setVertexAttribIPointer( index, array );
                }
                else
                {
                    state.setVertexAttribPointer( index, array );
                }
            }
        }
    }

    state.applyDisablingOfVertexAttributes();
}

#else

void
TileDrawable::drawVertexArraysImplementation(osg::RenderInfo& renderInfo) const
{
    State& state = *renderInfo.getState();
    bool handleVertexAttributes = !_geom->getVertexAttribArrayList().empty();
    //bool handleVertexAttributes = !_vertexAttribList.empty();

    ArrayDispatchers& arrayDispatchers = state.getArrayDispatchers();

    arrayDispatchers.reset();
    arrayDispatchers.setUseVertexAttribAlias(state.getUseVertexAttributeAliasing());
    arrayDispatchers.setUseGLBeginEndAdapter(false);

    arrayDispatchers.activateNormalArray(_geom->getNormalBinding(), _geom->getNormalArray(), _geom->getNormalIndices());
    arrayDispatchers.activateColorArray(_geom->getColorBinding(), _geom->getColorArray(), _geom->getColorIndices());
    arrayDispatchers.activateSecondaryColorArray(_geom->getSecondaryColorBinding(), _geom->getSecondaryColorArray(), _geom->getSecondaryColorIndices());
    arrayDispatchers.activateFogCoordArray(_geom->getFogCoordBinding(), _geom->getFogCoordArray(), _geom->getFogCoordIndices());

    if (handleVertexAttributes)
    {
        for(unsigned int unit=0;unit < _geom->getVertexAttribArrayList().size();++unit)
        {
            const osg::Geometry::ArrayData& val = _geom->getVertexAttribArrayList()[unit];
            arrayDispatchers.activateVertexAttribArray(val.binding, unit, val.array.get(), val.indices.get());
        }
    }

    // dispatch any attributes that are bound overall
    arrayDispatchers.dispatch(_geom->BIND_OVERALL, 0);

    state.lazyDisablingOfVertexAttributes();

    // set up arrays
    if( _geom->getVertexArray() )
        state.setVertexPointer(_geom->getVertexArray()); //_vertexData.array.get());

    if (_geom->getNormalBinding()==_geom->BIND_PER_VERTEX && _geom->getNormalArray())
        state.setNormalPointer(_geom->getNormalArray());

    if (_geom->getColorBinding()==_geom->BIND_PER_VERTEX && _geom->getColorArray())
        state.setColorPointer(_geom->getColorArray());

    if (_geom->getSecondaryColorBinding()==_geom->BIND_PER_VERTEX && _geom->getSecondaryColorArray())
        state.setSecondaryColorPointer(_geom->getSecondaryColorArray());

    if (_geom->getFogCoordBinding()==_geom->BIND_PER_VERTEX && _geom->getFogCoordArray())
        state.setFogCoordPointer(_geom->getFogCoordArray());
    
    for(unsigned int unit=0;unit<_geom->getTexCoordArrayList().size();++unit)
    {
        const Array* array = _geom->getTexCoordArray(unit);
        if (array)
        {
            state.setTexCoordPointer(unit,array);
        }
    }
    
    if ( handleVertexAttributes )
    {
        for(unsigned int index = 0; index < _geom->getVertexAttribArrayList().size(); ++index)
        {
            const osg::Array* array = _geom->getVertexAttribArray(index);
            if ( array && _geom->getVertexAttribBinding(index) == _geom->BIND_PER_VERTEX )
            {
                if (array->getPreserveDataType())
                {
                    GLenum dataType = array->getDataType();
                    if (dataType==GL_FLOAT) state.setVertexAttribPointer( index, array, GL_FALSE );
                    else if (dataType==GL_DOUBLE) state.setVertexAttribLPointer( index, array );
                    else state.setVertexAttribIPointer( index, array );
                }
                else
                {
                    state.setVertexAttribPointer( index, array, GL_FALSE );
                }
            }
        }
    }

    state.applyDisablingOfVertexAttributes();
}

#endif
