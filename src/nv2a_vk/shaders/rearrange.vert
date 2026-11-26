// One triangle over all of the surface.
#version 460

void main()
{
    gl_Position = vec4(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0,
                       0.0, 1.0);
}
