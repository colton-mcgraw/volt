# Text Rendering Shaders

This folder contains shaders used for rendering text in the UI. The text rendering pipeline uses a Vello style flattening approach. Glyphs are passed in as either cubic or quadratic bezier curves. 

First, the curves are converted to a cubic curve if necessary, any transforms are applied, and then flattened to line segments in a compute shader.

Next a "binning" shader determines which pixels are covered by the geometry and writes out a coarse coverage mask.

Then a coarse compute shader uses the coverage mask to determine which pixels need to be shaded and writes out a list of those pixels.

Finally a fine compute shader iterates over the list of pixels and determines the final coverage for each pixel.
