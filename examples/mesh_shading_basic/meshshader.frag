/* Copyright (c) 2021, Sascha Willems
 *
 * SPDX-License-Identifier: MIT
 *
 */

#version 450
#extension GL_EXT_mesh_shader : require
 
// layout (location = 0) in VertexInput {
//   vec4 color;
// } vertexInput;

layout(location = 1) perprimitiveEXT in PrimitiveInput {
  flat vec4 color;
} primitiveInput;

layout(location = 0) out vec4 outFragColor;
 

void main()
{
	outFragColor = primitiveInput.color;
}