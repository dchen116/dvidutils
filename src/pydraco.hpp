#ifndef DVIDUTILS_PYDRACO_HPP
#define DVIDUTILS_PYDRACO_HPP

#include <cstdint>
#include <tuple>
#include <algorithm>

#include "draco/mesh/mesh.h"
#include "draco/compression/encode.h"
#include "draco/compression/decode.h"

#include "pybind11/pybind11.h"

#include "xtensor/xmath.hpp"
#include "xtensor-python/pytensor.hpp"

using std::uint32_t;
using std::size_t;

namespace py = pybind11;

typedef xt::pytensor<float, 2> vertices_array_t;
typedef xt::pytensor<float, 2> normals_array_t;
typedef xt::pytensor<uint32_t, 2> faces_array_t;

int DRACO_SPEED = 0; // Best compression. See encode.h

// Encode the given vertices and faces arrays from python
// into a buffer (bytes object) encoded via draco.
//
// Special case: If faces is empty, an empty buffer is returned.
//
// Note: No support for vertex normals.
// Note: The vertices are expected to be passed in X,Y,Z order
py::bytes encode_faces_to_drc_bytes( vertices_array_t const & vertices,
                                     normals_array_t const & normals,
                                     faces_array_t const & faces )
{
    using namespace draco;

    auto vertex_count = vertices.shape()[0];
    auto normal_count = normals.shape()[0];
    auto face_count = faces.shape()[0];

    // Special case:
    // If faces is empty, an empty buffer is returned.
    if (face_count == 0)
    {
        return py::bytes();
    }
    
    int max_vertex = xt::amax(faces)();
    if (vertex_count < max_vertex+1)
    {
        throw std::runtime_error("Face indexes exceed vertices length");
    }
    
    if (normal_count > 0 and normal_count != vertex_count)
    {
        throw std::runtime_error("normals array size does not correspond to vertices array size");
    }

    Mesh mesh;
    mesh.set_num_points(vertex_count);
    mesh.SetNumFaces(face_count);

    // Init vertex attribute
    PointAttribute vert_att_template;
    vert_att_template.Init( GeometryAttribute::POSITION,    // attribute_type
                            nullptr,                        // buffer
                            3,                              // num_components
                            DT_FLOAT32,                     // data_type
                            false,                          // normalized
                            DataTypeLength(DT_FLOAT32) * 3, // byte_stride
                            0 );                            // byte_offset
    vert_att_template.SetIdentityMapping();

    // Add vertex attribute to mesh (makes a copy internally)
    int vert_att_id = mesh.AddAttribute(vert_att_template, true, vertex_count);
    mesh.SetAttributeElementType(vert_att_id, MESH_VERTEX_ATTRIBUTE);

    // Get a reference to the mesh's copy of the vertex attribute
    PointAttribute & vert_att = *(mesh.attribute(vert_att_id));

    // Load the vertices into the vertex attribute
    for (size_t vi = 0; vi < vertex_count; ++vi)
    {
        std::array<float, 3> v{{ vertices(vi, 0), vertices(vi, 1), vertices(vi, 2) }};
        vert_att.SetAttributeValue(AttributeValueIndex(vi), v.data());
    }
    
    if (normal_count > 0)
    {
        // Init normal attribute
        PointAttribute norm_att_template;
        norm_att_template.Init( GeometryAttribute::NORMAL,      // attribute_type
                                nullptr,                        // buffer
                                3,                              // num_components
                                DT_FLOAT32,                     // data_type
                                false,                          // normalized
                                DataTypeLength(DT_FLOAT32) * 3, // byte_stride
                                0 );                            // byte_offset
        norm_att_template.SetIdentityMapping();
        
        // Add normal attribute to mesh (makes a copy internally)
        int norm_att_id = mesh.AddAttribute(norm_att_template, true, normal_count);
        mesh.SetAttributeElementType(norm_att_id, MESH_VERTEX_ATTRIBUTE);

        // Get a reference to the mesh's copy of the normal attribute
        PointAttribute & norm_att = *(mesh.attribute(norm_att_id));
        
        // Load the normals into the normal attribute
        for (size_t ni = 0; ni < normal_count; ++ni)
        {
            std::array<float, 3> n{{ normals(ni, 0), normals(ni, 1), normals(ni, 2) }};
            norm_att.SetAttributeValue(AttributeValueIndex(ni), n.data());
        }
    }
    
    // Load the faces
    for (size_t f = 0; f < face_count; ++f)
    {
        Mesh::Face face = {{ PointIndex(faces(f, 0)),
                             PointIndex(faces(f, 1)),
                             PointIndex(faces(f, 2)) }};
        
        for (auto vi : face)
        {
            assert(vi < vertex_count && "face has an out-of-bounds vertex");
        }
        
        mesh.SetFace(draco::FaceIndex(f), face);
    }
    
    mesh.DeduplicateAttributeValues();
    mesh.DeduplicatePointIds();
    
    draco::EncoderBuffer buf;
    draco::Encoder encoder;
    encoder.SetSpeedOptions(DRACO_SPEED, DRACO_SPEED);
    encoder.EncodeMeshToBuffer(mesh, &buf);
    
    return py::bytes(buf.data(), buf.size());
}

// Decode a draco-encoded buffer (given as a python bytes object)
// into a xtensor-python arrays for the vertices and faces
// (which are converted to numpy arrays on the python side).
//
// Special case: If drc_bytes is empty, return empty vertices and faces.
//
// Note: normals are not decoded (currently)
// Note: The vertexes are returned in X,Y,Z order.
std::tuple<vertices_array_t, normals_array_t, faces_array_t> decode_drc_bytes_to_faces( py::bytes const & drc_bytes )
{
    using namespace draco;
    
    // Special case:
    // If drc_bytes is empty, return empty vertices and faces.
    if (py::len(drc_bytes) == 0)
    {
        vertices_array_t::shape_type verts_shape = {{0, 3}};
        vertices_array_t vertices(verts_shape);

        normals_array_t::shape_type normals_shape = {{0, 3}};
        normals_array_t normals(normals_shape);
        
        faces_array_t::shape_type faces_shape = {{0, 3}};
        faces_array_t faces(faces_shape);

        return std::make_tuple( std::move(vertices), std::move(normals), std::move(faces) );
    }

    // Extract pointer to raw bytes (avoid copy)
    PyObject * pyObj = drc_bytes.ptr();
    char * raw_buf = nullptr;
    Py_ssize_t bytes_length = 0;
    PyBytes_AsStringAndSize(pyObj, &raw_buf, &bytes_length);
    
    // Wrap bytes in a DecoderBuffer
    DecoderBuffer buf;
    buf.Init( raw_buf, bytes_length );
    
    // Decode to Mesh
    Decoder decoder;
    
    auto geometry_type = decoder.GetEncodedGeometryType(&buf).value();
    if (geometry_type != TRIANGULAR_MESH)
    {
        throw std::runtime_error("Buffer does not appear to be a mesh file. (Is it a pointcloud?)");
    }
    
    auto pMesh = decoder.DecodeMeshFromBuffer(&buf).value();
    
    // Strangely, encoding a mesh may cause it to have duplicate point ids,
    // so we should de-duplicate them after decoding.
    pMesh->DeduplicateAttributeValues();
    pMesh->DeduplicatePointIds();

    int point_count = pMesh->num_points();
    
    // Extract vertices
    const PointAttribute *const vertex_att = pMesh->GetNamedAttribute(GeometryAttribute::POSITION);
    if (vertex_att == nullptr)
    {
        throw std::runtime_error("Draco mesh appears to have no vertices.");
    }
    
    vertices_array_t::shape_type verts_shape = {{point_count, 3}};
    vertices_array_t vertices(verts_shape);
    
    std::array<float, 3> vertex_value;
    for (PointIndex i(0); i < point_count; ++i)
    {
        if (!vertex_att->ConvertValue<float, 3>(vertex_att->mapped_index(i), &vertex_value[0]))
        {
            std::ostringstream ssErr;
            ssErr << "Error reading vertex " << i.value() << std::endl;
            throw std::runtime_error(ssErr.str());
        }
        vertices(i.value(), 0) = vertex_value[0];
        vertices(i.value(), 1) = vertex_value[1];
        vertices(i.value(), 2) = vertex_value[2];
    }

    // Extract normals (if any)
    const PointAttribute *const normal_att = pMesh->GetNamedAttribute(GeometryAttribute::NORMAL);
    normals_array_t::shape_type::value_type normal_count = 0;
    if (normal_att == nullptr)
    {
        normal_count = 0;
    }
    else
    {
        // See Note below about why we don't use normal_att->size()
        normal_count = point_count;
    }
    
    normals_array_t::shape_type normals_shape = {{normal_count, 3}};
    normals_array_t normals(normals_shape);
    
    if (normal_count > 0)
    {
        std::array<float, 3> normal_value;
        
        // Important:
        // We don't use normal_att->size(), because it might be smaller
        // than the number of vertices (if not all vertices had unique normals).
        // Instead, we loop over the POINT indices, mapping from point indices to normal entries.
        for (PointIndex i(0); i < normal_count; ++i)
        {
            if (!normal_att->ConvertValue<float, 3>(normal_att->mapped_index(i), &normal_value[0]))
            {
                std::ostringstream ssErr;
                ssErr << "Error reading normal for point " << i << std::endl;
                throw std::runtime_error(ssErr.str());
            }
            normals(i.value(), 0) = normal_value[0];
            normals(i.value(), 1) = normal_value[1];
            normals(i.value(), 2) = normal_value[2];
        }
    }

    // Extract faces
    faces_array_t::shape_type faces_shape = {{pMesh->num_faces(), 3}};
    faces_array_t faces(faces_shape);
    
    for (auto i = 0; i < pMesh->num_faces(); ++i)
    {
        auto const & face = pMesh->face(FaceIndex(i));
        faces(i, 0) = face[0].value();
        faces(i, 1) = face[1].value();
        faces(i, 2) = face[2].value();
    }
    

    return std::make_tuple( std::move(vertices), std::move(normals), std::move(faces) );
}

#endif
