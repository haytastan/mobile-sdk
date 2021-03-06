/*
 * Copyright (c) 2016 CartoDB. All rights reserved.
 * Copying and using this code is allowed only according
 * to license terms, as given in https://cartodb.com/terms/
 */

#ifndef _CARTO_POLYGON3DDRAWDATA_H_
#define _CARTO_POLYGON3DDRAWDATA_H_

#include "renderers/drawdatas/VectorElementDrawData.h"

#include <memory>
#include <vector>

#include <cglib/vec.h>
#include <cglib/bbox.h>

namespace carto {
    class Polygon3D;
    class Polygon3DStyle;
    class Projection;
    class ProjectionSurface;
    
    class Polygon3DDrawData : public VectorElementDrawData {
    public:
        Polygon3DDrawData(const Polygon3D& polygon3D, const Polygon3DStyle& style, const Projection& projection, const ProjectionSurface& projectionSurface);
        virtual ~Polygon3DDrawData();

        const Color& getSideColor() const;
    
        const cglib::bbox3<double>& getBoundingBox() const;
    
        const std::vector<cglib::vec3<double> >& getCoords() const;
    
        const std::vector<cglib::vec3<float> >& getNormals() const;

        const std::vector<unsigned char>& getAttribs() const;
    
        virtual void offsetHorizontally(double offset);
    
    private:
        Color _sideColor;
    
        cglib::bbox3<double> _boundingBox;
    
        std::vector<cglib::vec3<double> > _coords;

        std::vector<cglib::vec3<float> > _normals;

        std::vector<unsigned char> _attribs;
    };
    
}

#endif
