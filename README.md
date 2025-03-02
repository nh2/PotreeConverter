
# About

PotreeConverter generates an octree LOD structure for streaming and real-time rendering of massive point clouds. The results can be viewed in web browsers with [Potree](https://github.com/potree/potree) or as a desktop application with [PotreeDesktop](https://github.com/potree/PotreeDesktop). 

Version 2.0 is a complete rewrite with following differences over the previous version 1.7:

* About 10 to 50 times faster than PotreeConverter 1.7 on SSDs.
* Produces a total of 3 files instead of thousands to tens of millions of files. The reduction of the number of files improves file system operations such as copy, delete and upload to servers from hours and days to seconds and minutes. 
* Better support for standard LAS attributes and arbitrary extra attributes. Full support (e.g. int64 and uint64) in development.
* Optional compression is not yet available in the new converter but on the roadmap for a future update.

Altough the converter made a major step to version 2.0, the format it produces is also supported by Potree 1.7. The Potree viewer is scheduled to make the major step to version 2.0 in 2021, with a rewrite in WebGPU. 

# Publications

* [Potree: Rendering Large Point Clouds in Web Browsers](https://www.cg.tuwien.ac.at/research/publications/2016/SCHUETZ-2016-POT/SCHUETZ-2016-POT-thesis.pdf)
* [Fast Out-of-Core Octree Generation for Massive Point Clouds](https://www.cg.tuwien.ac.at/research/publications/2020/SCHUETZ-2020-MPC/), _Schütz M., Ohrhallinger S., Wimmer M._

# Getting Started

1. Download windows binaries or
    * Download source code
	* Install [CMake](https://cmake.org/) 3.16 or later
	* Create and jump into folder "build"
	    ```
	    mkdir build
	    cd build
	    ```
	* run 
	    ```
	    cmake ../
	    ```
	* On linux, run: ```make```
	* On windows, open Visual Studio 2019 Project ./Converter/Converter.sln and compile it in release mode
2. run ```PotreeConverter.exe <input> -o <outputDir>```
    * Optionally specify the sampling strategy:
	* Poisson-disk sampling (default): ```PotreeConverter.exe <input> -o <outputDir> -m poisson```
	* Random sampling: ```PotreeConverter.exe <input> -o <outputDir> -m random```

In Potree, modify one of the examples with following load command:

```javascript
let url = "../pointclouds/D/temp/test/metadata.json";
Potree.loadPointCloud(url).then(e => {
	let pointcloud = e.pointcloud;
	let material = pointcloud.material;

	material.activeAttributeName = "rgba";
	material.minSize = 2;
	material.pointSizeType = Potree.PointSizeType.ADAPTIVE;

	viewer.scene.addPointCloud(pointcloud);
	viewer.fitToScreen();
});

```

# Alternatives

PotreeConverter 2.0 produces a very different format than previous iterations. If you find issues, you can still try previous converters or alternatives:

<table>
	<tr>
		<th></th>
		<th>PotreeConverter 2.0</th>
		<th><a href="https://github.com/potree/PotreeConverter/releases/tag/1.7">PotreeConverter 1.7</a></th>
		<th><a href="https://entwine.io/">Entwine</a></th>
	</tr>
	<tr>
		<th>license</th>
		<td>
			free, BSD 2-clause
		</td>
		<td>
			free, BSD 2-clause
		</td>
		<td>
			free, LGPL
		</td>
	</tr>
	<tr>
		<th>#generated files</th>
		<td>
			3 files total
		</td>
		<td>
			1 per node
		</td>
		<td>
			1 per node
		</td>
	</tr>
	<tr>
		<th>compression</th>
		<td>
			none (TODO)
		</td>
		<td>
			LAZ (optional)
		</td>
		<td>
			LAZ
		</td>
	</tr>
</table>

Performance comparison (Ryzen 2700, NVMe SSD):

![](./docs/images/performance_chart.png)

# Code overview

`PotreeConverter` converts a list of LAS point cloud files into a hierarchical multi-resolution octree.

The multi-resolution octree gives the Potree viewer the ability to load more or less points depending on distance, the amount of octree nodes in the view frustum, and the user's desired point load limit.

`main()` does 2 key steps (each explained in detail further down):

* `chunking()`: Distributing the points into an on-disk octree with each node's file containing roughly equally many points (`<= maxPointsPerChunk`).
* `indexing()`: Creating subsampled views of the octree (making it multi-resolution), concatenating all nodes' points into a large file `octree.bin`, and creating a smaller file `hierarchy.bin` which tells for any given (node, resolution) the byte range in `octree.bin` where its points can be found.

You may perform them separately by invoking `PotreeConverter` first with `--keep-chunks --no-indexing`, and then with `--no-chunking`.

We perform the following:

* Read the headers of all LAS files to extract from each:
  Point types, offsets, scale factors, and min/max point bounding boxes.
  See `loadLasHeader()`.
  * Note LAS files store points as quantised 32-bit integers.
    The formula to compute a point's coordinate is then e.g. `x = las_int32_x * las_scale_x + las_offset_x`.
    A typical `scale` factor of a LAS file is `0.001` which allows to express millimeter resolution.
* Compute an overall scale factor and bounding box to use for the joint potree output.
  See `computeScaleOffset()`.
* Chunking in `doChunking()`:
  * Divide the bounding box into a regular `grid` of `gridSize = 512`³ many cells.
    If there are few points, the grid size may be smaller.
  * Log output `COUNTING` (`countPointsInCells()`):
    For each cell, count how many input points fall into it.
    Done in a parallel streaming read across the LAS files.
  * Distributing:
  	* `createLUT()`:
	  * "Merge up" the grid into an octree, merging any 8 octants into 1 larger one while the merged point count is `<= maxPointsPerChunk`.
      * A final (unmergeable) octree node with its contained points is called a "chunk".
      * Compute a lookup-table which maps each original grid (x,y,z) cell index to the node index the cell was merged into.
    * Log output `CREATING CHUNKS` (`distributePoints()`):
      Write points into one file per chunk, e.g. `outdir/chunks/r42410.bin`, `r0044.bin`, etc.
      * Done in a parallel streaming read across the LAS files, to read points and append them to their `.bin` file.
      * The lookup-table is used to identify the octree node that the point belongs into, in constant time.
      * Each digit in the file name identifies the quadrant of the octree to traverse at the corresponding level, down to the node.
      * Writing is done through the `ConcurrentWriter()` class, which buffers the data to write to each file in memory, and uses a thread pool to write it to disk.
* Indexing in `doIndexing()` (log output `INDEXING`):
  * Based on the user's chosen point sampling method (`poisson`, `poisson_average`, `random`), the corresponding subclass of `Sampler` is instantiated.
  	* `random` sampling is fast and simple. The other methods try to select more visually pleasing subsamples via poisson-disk sampling.
  * `TODO`: Detail further how indexing works.
  * Delete the `chunks/` dir.

# License 

PotreeConverter is available under the [BSD 2-clause license](./LICENSE).