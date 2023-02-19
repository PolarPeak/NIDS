import * as d3 from 'd3'
import * as THREE from 'three'

import {
    Line2
} from 'three/examples/jsm/lines/Line2'
import {
    LineGeometry
} from 'three/examples/jsm/lines/LineGeometry'
import {
    LineMaterial
} from 'three/examples/jsm/lines/LineMaterial'

import {
    FontLoader
} from 'three/examples/jsm/loaders/FontLoader'
import {
    TextGeometry
} from 'three/examples/jsm/geometries/TextGeometry'

const OrbitControls = require('three-orbit-controls')(THREE)
// 使用 controls = new OrbitControls(camera);

// import { MTLLoader, OBJLoader } from 'three-obj-mtl-loader'
// 使用 mtlLoader = new MTLLoader(); objLoader = new OBJLoader();

// import { CSS2DObject, CSS2DRenderer } from 'three-css2drender'
// 使用：labelRenderer = new CSS2DRenderer(); label = new CSS2DObject(text);

import CenterPoint from './CenterPoint'

const font = require("@/assets/fonts/STSong_Regular.json");

const textureNormal = new THREE.TextureLoader().load(require("@/assets/images/map/textureNormal.jpg"))
const calloutText = new THREE.TextureLoader().load(require("@/assets/images/map/callout.png"))



/**
 * Three平面3D地图实现类
 */
export default class ThreeMapV2Impl {
    constructor({
        domElement,
        mapName,
        mapData,
        option
    }) {
        this.scene
        this.camera
        this.renderer
        this.controls
        this.mapModel
        // 墨卡托投影
        this.projection
        this.lonLatToVectorData

        this.option = option
        this.mapData = mapData
        this.mapName = mapName
        this.domElement = domElement
        this.gridHelperName = "网格辅助"
        this.axesHelperName = "坐标辅助"
        this.fontLoader = new FontLoader();

        // 光柱散点速度
        this.lightBeamScatterSpeed = 0.02
        // 飞行线速度
        this.flightLineSpeed = 0.1

        this.waveMeshArr = []
        // this.flightLineTexture = new THREE.TextureLoader().load(require("@/renderer/resources/images/map/stroke2.png"))
        this.flightLineTexture = new THREE.TextureLoader().load(require("@/assets/images/map/stroke3.png"))


    }
    //============================================================>
    /**
  * 
  * @param {*} mapData 
  */
    initMapBorderByGeoJson(border, mapData) {

        const fMapBorder = this.scene.getObjectByName("地图描边");
        if (fMapBorder) {
            this.scene.remove(fMapBorder)
        }

        const mapBorder = new THREE.Group()
        mapBorder.name = '地图描边'
        mapData.features.forEach(item => {
            const areas = item.geometry.coordinates
            const areaGroup = new THREE.Group()
            areaGroup.name = `区域组-${item.properties.name}`
            const lineGroup = new THREE.Group()
            lineGroup.name = '描边线段组'
            /**
             * 绘制描边线段
             */
            this.lonLatToVectorData = areas.map((area) => {
                var isOut = false
                area = area.map((print, index) => {
                    if (print[index] instanceof Array) {
                        print = print.map(printInner => {
                            printInner = this.lonLatToVector(printInner)
                            return printInner
                        })
                        if (border.up.show) {
                            const lineUp = this.initStrokedLineSegmentBorder(print, border.up)
                            lineGroup.add(lineUp)
                        }
                        if (border.down.show) {
                            const lineDown = this.initStrokedLineSegmentBorder(print, border.down)
                            lineDown.position.set(0, 0, this.option.depth || 10)
                            lineGroup.add(lineDown)
                        }
                    } else {
                        isOut = true, print = this.lonLatToVector(print)
                    }
                    return print
                })
                if (isOut) {
                    if (border.up.show) {
                        const lineUp = this.initStrokedLineSegmentBorder(area, border.up)
                        lineGroup.add(lineUp)
                    }
                    if (border.down.show) {
                        const lineDown = this.initStrokedLineSegmentBorder(area, border.down)
                        lineDown.position.set(0, 0, this.option.depth || 10)
                        lineGroup.add(lineDown)
                    }
                }
                return area
            })
            areaGroup.rotation.x = Math.PI
            areaGroup.add(lineGroup)
            mapBorder.add(areaGroup)
        })
        this.scene.add(mapBorder)
    }
    // | 绘制描边线段
    initStrokedLineSegmentBorder(lines, border) {
        const lineData = [];
        lines.forEach(p => lineData.push(p[0], p[1], 0))
        const geometry = new LineGeometry()
        geometry.setPositions(new Float32Array(lineData))
        const lineMaterial = new LineMaterial({
            transparent: true,
            color: border.color,
            linewidth: border.width,
            opacity: border.opacity
        })
        lineMaterial.resolution.set(this.domElement.offsetWidth, this.domElement.offsetHeight)
        const line = new Line2(geometry, lineMaterial)
        line.name = '描边线段'
        line.computeLineDistances()
        return line
    }
    //============================================================>
    /**
     * 初始化地图 使用GeoJson数据绘制
     * @param {*} mapData 
     */
    initMapByGeoJson(mapData) {
        this.mapModel = new THREE.Group()
        this.mapModel.name = '地图'
        mapData.features.forEach(item => {
            const areas = item.geometry.coordinates
            const areaGroup = new THREE.Group()
            areaGroup.name = `区域组-${item.properties.name}`
            const plateGroup = new THREE.Group()
            plateGroup.name = '地图板块组'
            const lineGroup = new THREE.Group()
            lineGroup.name = '描边线段组'
            const fontGroup = new THREE.Group()
            fontGroup.name = '标签文字组'

            if (this.option.label.show) {
                /**
                 * 绘制区域文字标签
                 */
                try {
                    const fontGeometry = new TextGeometry(item.properties.name, {
                        font: this.fontLoader.parse(font),
                        size: 0.8,
                        height: 0.1
                    });
                    const fontMeshMaterial = new THREE.MeshPhongMaterial({
                        opacity: this.option.label.opacity || 1,
                        color: this.option.label.color || '#007cff',
                        transparent: true
                    });
                    const fontMesh = new THREE.Mesh(fontGeometry, fontMeshMaterial);
                    const [x, y, z] = this.lonLatToVector(item.properties.center)
                    fontMesh.position.set(x, y, z);
                    fontMesh.name = '标签'
                    fontMesh.rotation.x = -Math.PI
                    fontGroup.add(fontMesh)
                } catch (error) {
                    console.warn("fontGroup", "标签文字设置错误！！！")
                }
            }

            /**
             * 绘制区域板块&描边线段
             */
            this.lonLatToVectorData = areas.map((area) => {
                var isOut = false
                area = area.map((print, index) => {
                    if (print[index] instanceof Array) {
                        print = print.map(printInner => {
                            printInner = this.lonLatToVector(printInner)
                            return printInner
                        })
                        plateGroup.add(this.initAreaTilesMesh(print))
                        const lineUp = this.initStrokedLineSegment(print)
                        const lineDown = lineUp.clone()
                        lineDown.position.set(0, 0, this.option.depth || 10)
                        lineGroup.add(lineUp, lineDown)
                    } else {
                        isOut = true, print = this.lonLatToVector(print)
                    }
                    return print
                })
                if (isOut) {
                    plateGroup.add(this.initAreaTilesMesh(area))

                    const lineUp = this.initStrokedLineSegment(area)
                    const lineDown = lineUp.clone()
                    lineDown.position.set(0, 0, this.option.depth || 10)
                    lineGroup.add(lineUp, lineDown)
                }
                return area
            })
            areaGroup.rotation.x = Math.PI
            areaGroup.add(plateGroup, lineGroup, fontGroup)
            this.mapModel.add(areaGroup)
        })
        this.scene.add(this.mapModel)
    }
    // | 绘制区域板块
    initAreaTilesMesh(points) {
        const shape = new THREE.Shape()
        points.forEach((p, i) => {
            const [x, y] = p;
            if (i === 0) {
                shape.moveTo(x, y)
            } else if (i === points.length - 1) {
                shape.quadraticCurveTo(x, y, x, y)
            } else {
                shape.lineTo(x, y, x, y)
            }
        })
        const areaGeometry = new THREE.ExtrudeGeometry(shape, {
            depth: this.option.depth || 10,
            bevelEnabled: false
        })

        const areaMateria = new THREE.MeshPhongMaterial({
            color: this.option.area.color || '#007cff',
            opacity: this.option.area.opacity || 0.8,
            transparent: true,
        })

        if (this.option.area.normal.show) {
            textureNormal.wrapS = textureNormal.wrapT = THREE.RepeatWrapping;
            textureNormal.repeat.set(this.option.area.normal.repeat, this.option.area.normal.repeat);
            areaMateria.normalMap = textureNormal
            areaMateria.normalScale = new THREE.Vector2(this.option.area.normal.normalScale, this.option.area.normal.normalScale)
        }

        const areaMesh = new THREE.Mesh(areaGeometry, areaMateria)
        areaMesh.name = '区域板块'
        return areaMesh
    }
    // | 绘制描边线段
    initStrokedLineSegment(lines) {
        const lineData = [];
        lines.forEach(p => lineData.push(p[0], p[1], 0))
        const geometry = new LineGeometry()
        geometry.setPositions(new Float32Array(lineData))
        const lineMaterial = new LineMaterial({
            transparent: true,
            color: this.option.line.color || '#ffffff',
            linewidth: this.option.line.linewidth || 1,
            opacity: this.option.line.opacity || 0.8
        })
        lineMaterial.resolution.set(this.domElement.offsetWidth, this.domElement.offsetHeight)
        const line = new Line2(geometry, lineMaterial)
        line.name = '描边线段'
        line.computeLineDistances()
        return line
    }
    /**
     *  经纬度转墨卡托投影
     * @param {*} lonLat 
     * @returns 
     */
    lonLatToVector(lonLat) {

        const center = CenterPoint.find(item => item.value == this.mapName)

        this.option.scale = center.scale

        if (!this.projection) {
            this.projection = d3
                .geoMercator()
                .center(center.center)
                .scale(this.option.scale)
                .translate([0, 0]);
        }
        const [y, x] = this.projection([...lonLat])
        
        return [y, x, 0]
    }
    // | 初始化网格辅助
    initGridHelper(gridHelper) {
        if (gridHelper) {
            this.option.gridHelper = gridHelper
        }
        if (this.option.gridHelper.show) this.openGridHelper()
        if (!this.option.gridHelper.show) this.closeGridHelper()
    }
    // | 开启网格辅助
    openGridHelper() {
        if (this.option.gridHelper.show) {
            // 网格辅助
            const gh_width = this.option.gridHelper.width || 300
            const gh_height = this.option.gridHelper.height || 300
            const gh_x_color = this.option.gridHelper.xColor || 0xffffff
            const gh_y_color = this.option.gridHelper.yColor || 0xffffff
            this.option.gridHelper.material = this.option.gridHelper.material || {}
            const gh_material_opacity = this.option.gridHelper.material.opacity || 0.5
            const gh_material_transparent = this.option.gridHelper.material.transparent || true
            const gridHelper = new THREE.GridHelper(gh_width, gh_height, gh_x_color, gh_y_color);
            gridHelper.material.opacity = gh_material_opacity;
            gridHelper.material.transparent = gh_material_transparent;
            gridHelper.name = this.gridHelperName
            this.closeGridHelper()
            this.scene.add(gridHelper);
        }
    }
    // | 关闭网格辅助
    closeGridHelper() {
        const gridHelper = this.scene.getObjectByName(this.gridHelperName);
        if (gridHelper) {
            this.scene.remove(gridHelper)
        }
    }
    // | 初始化坐标辅助
    initAxesHelper(axesHelper) {
        if (axesHelper) {
            this.option.axesHelper = axesHelper
        }
        if (this.option.axesHelper.show) this.openAxesHelper()
        if (!this.option.axesHelper.show) this.closeAxesHelper()
    }
    // | 开启坐标辅助
    openAxesHelper() {
        if (this.option.axesHelper.show) {
            const ah_size = this.option.axesHelper.size || 100
            const axesHelper = new THREE.AxesHelper(ah_size)
            axesHelper.name = this.axesHelperName
            this.closeAxesHelper()
            this.scene.add(axesHelper)
        }
    }
    // | 关闭坐标辅助
    closeAxesHelper() {
        const axesHelper = this.scene.getObjectByName(this.axesHelperName);
        if (axesHelper) {
            this.scene.remove(axesHelper)
        }
    }
    // | 初始化控制器
    initControls() {
        this.controls = new OrbitControls(this.camera, this.domElement);
        this.controls.update()
    }
    /**
     * 设置地图的绝对位置信息
     * @param {*} option 
     */
    setMapPosition(option) {
        this.option.x = option.x, this.option.y = option.y, this.option.z = option.z
        this.mapModel.position.set(this.option.x, this.option.y, this.option.z);
    }
    /**
     * 设置相机的绝对位置信息
     * @param {*} option 
     */
    setCameraPosition(option) {
        this.option.angleX = option.angleX, this.option.angleY = option.angleY, this.option.distance = option.distance
        this.camera.position.set(this.option.angleX, this.option.angleY, this.option.distance);
        this.camera.lookAt(0, 0, 0);
    }
    /**
     * 设置平行光
     * @param {*} option 
     */
    setDirectionalLight(option) {
        this.directionalLight.position.set(option.x, option.y, option.z);
        this.directionalLight.lookAt(0, 0, 0)
        this.directionalLight.color.set(option.color)
        this.directionalLight.intensity = option.intensity

        const directionalLightHelper = this.scene.getObjectByName("directionalLightHelper");
        if (directionalLightHelper) {
            this.scene.remove(directionalLightHelper)
        }

        if (option.helper.show) {
            this.initDirectionalLightHelper()
        }
    }
    /**
     * 设置环境光
     * @param {*} option 
     */
    setAmbientLight(option) {
        this.ambientLight.color.set(option.color)
        this.ambientLight.intensity = option.intensity
    }
    // | 重置大小
    resize() {
        // 重置渲染器输出画布canvas尺寸
        this.renderer.setSize(this.domElement.offsetWidth, this.domElement.offsetHeight);
        // 全屏情况下：设置观察范围长宽比aspect为窗口宽高比
        this.camera.aspect = this.domElement.offsetWidth / this.domElement.offsetHeight;
        // 渲染器执行render方法的时候会读取相机对象的投影矩阵属性projectionMatrix
        // 但是不会每渲染一帧，就通过相机的属性计算投影矩阵(节约计算资源)
        // 如果相机的一些属性发生了变化，需要执行updateProjectionMatrix ()方法更新相机的投影矩阵
        this.camera.updateProjectionMatrix();
    }

    /**
     * 绘制光柱
     * @param {*} data 
     */
    setDrawBeamLight(data, name, {
        radius = 1,
        baseHeight = 0.2,
        speed = 0.02,
        material
    }) {
        const lightBeamScatter = this.scene.getObjectByName("lightBeamScatter_" + name);
        if (lightBeamScatter) {
            this.scene.remove(lightBeamScatter)
        }
        this.lightBeamScatterSpeed = speed
        material = material || {}
        const material_color = material.color || 'red'
        const material_opacity = material.opacity || 1

        const lightColumnTexture = new THREE.TextureLoader().load(material.lightColumnTexture)
        const calloutTexture = new THREE.TextureLoader().load(material.calloutTexture)
        const calloutApertureTexture = new THREE.TextureLoader().load(material.calloutApertureTexture);

        const lightGroup = new THREE.Group();

        data.forEach(item => {

            const [x, y, z] = this.lonLatToVector(item.value)

            function createPointMesh() {
                const geometry = new THREE.PlaneGeometry(radius, item.value[2] * baseHeight)
                const beamLightMaterial = new THREE.MeshBasicMaterial({
                    map: lightColumnTexture,
                    color: material_color, //光柱颜色，光柱map贴图是白色，可以通过color调节颜色
                    opacity: material_opacity,
                    transparent: true, //使用背景透明的png贴图，注意开启透明计算
                    side: THREE.DoubleSide, //双面可见
                    depthWrite: false, //是否对深度缓冲区有任何的影响
                })
                const mesh = new THREE.Mesh(geometry, beamLightMaterial)
                mesh.rotation.set(-Math.PI / 2, 0, 0)
                mesh.position.set(x, y, -(item.value[2] * baseHeight) / 2)

                return mesh
            }

            function createLightWaveMesh() {
                var geometry = new THREE.PlaneBufferGeometry(1, 1); //默认在XOY平面上
                var material = new THREE.MeshBasicMaterial({
                    color: material_color, //光柱颜色，光柱map贴图是白色，可以通过color调节颜色
                    opacity: material_opacity,
                    map: calloutTexture,
                    transparent: true, // 使用背景透明的png贴图，注意开启透明计算
                    side: THREE.DoubleSide, // 双面可见
                    depthWrite: false, // 禁止写入深度缓冲区数据
                });
                var mesh = new THREE.Mesh(geometry, material);
                var size = 3 //矩形平面Mesh的尺寸
                mesh.scale.set(size, size, size); //设置mesh大小
                mesh.position.set(x, y, -0.1)
                return mesh;
            }

            function createWaveMesh() {
                var geometry = new THREE.PlaneBufferGeometry(1, 1); //默认在XOY平面上
                var material = new THREE.MeshBasicMaterial({
                    color: material_color, //光柱颜色，光柱map贴图是白色，可以通过color调节颜色
                    opacity: material_opacity,
                    map: calloutApertureTexture,
                    transparent: true, //使用背景透明的png贴图，注意开启透明计算
                    side: THREE.DoubleSide, //双面可见
                    depthWrite: false, //禁止写入深度缓冲区数据
                });

                var mesh = new THREE.Mesh(geometry, material);
                var size = 3; //矩形平面Mesh的尺寸
                mesh.size = size; //自顶一个属性，表示mesh静态大小
                mesh.scale.set(size, size, size); //设置mesh大小
                mesh.position.set(x, y, -0.1);


                mesh._s = Math.random() * 1.0 + 1.0; //自定义属性._s表示mesh在原始大小基础上放大倍数  光圈在原来mesh.size基础上1~2倍之间变化
                return mesh;
            }

            const pointMesh = createPointMesh(),
                lightWaveMesh = createLightWaveMesh(),
                waveMesh = createWaveMesh()

            this.waveMeshArr.push(waveMesh);

            lightGroup.add(pointMesh, pointMesh.clone().rotateY(Math.PI / 2), lightWaveMesh, waveMesh)


        })
        lightGroup.name = "lightBeamScatter_" + name
        lightGroup.rotation.x = Math.PI

        this.scene.add(lightGroup)
    }

    /**
     * 绘制飞行线段
     * @param {*} data 
     * @param {*} name 
     * @param {*} option 
     */
    setDrawFlightLine(data, name, option) {

        const flightLine = this.scene.getObjectByName("flightLine_" + name);
        if (flightLine) {
            this.scene.remove(flightLine)
        }

        this.flightLineSpeed = option.line.speed

        const group = new THREE.Group()
        const flightLineGroup = new THREE.Group()
        const flightLineWaveGroup = new THREE.Group()

        data.forEach(item => {
            const start = this.lonLatToVector(item.coords[0])
            const end = this.lonLatToVector(item.coords[1])

            function createFlightLine(flightLineTexture) {
                flightLineTexture.wrapS = flightLineTexture.wrapT = THREE.RepeatWrapping;
                flightLineTexture.repeat.set(1, 1)
                flightLineTexture.needsUpdate = true
                const flightLineCurve = new THREE.QuadraticBezierCurve3(
                    new THREE.Vector3(start[0], 0, start[1]),
                    new THREE.Vector3(start[0] / 2, 20, 0),
                    new THREE.Vector3(end[0], 0, end[1])
                );
                const flightLineTubeGeometry = new THREE.TubeGeometry(flightLineCurve, option.line.twisty, option.line.width)
                const flightLineMaterial = new THREE.MeshBasicMaterial({
                    map: flightLineTexture,
                    color: option.line.color,
                    opacity: option.line.opacity,
                    side: THREE.DoubleSide, //双面可见
                    depthWrite: false, //禁止写入深度缓冲区数据
                    transparent: true
                })
                const flightLineMesh = new THREE.Mesh(flightLineTubeGeometry, flightLineMaterial);
                flightLineMesh.rotation.x = Math.PI / 2
                return flightLineMesh
            }

            function createLightWaveMesh(x, y, {
                color,
                opacity,
                size
            }) {
                var geometry = new THREE.PlaneBufferGeometry(1, 1); //默认在XOY平面上
                var material = new THREE.MeshBasicMaterial({
                    color: color, //光柱颜色，光柱map贴图是白色，可以通过color调节颜色
                    opacity: opacity,
                    map: calloutText,
                    transparent: true, // 使用背景透明的png贴图，注意开启透明计算
                    side: THREE.DoubleSide, // 双面可见
                    depthWrite: false, // 禁止写入深度缓冲区数据
                });
                var mesh = new THREE.Mesh(geometry, material);
                var size = size //矩形平面Mesh的尺寸
                mesh.scale.set(size, size, size); //设置mesh大小
                mesh.position.set(x, y, -0.1)
                return mesh;
            }

            const flightLineMesh = createFlightLine(this.flightLineTexture)

            const startLightWaveMesh = createLightWaveMesh(start[0], start[1], {
                size: option.scatterStart.size,
                color: option.scatterStart.color,
                option: option.scatterStart.option,
            })
            const endLightWaveMesh = createLightWaveMesh(end[0], end[1], {
                size: option.scatterEnd.size,
                color: option.scatterEnd.color,
                option: option.scatterEnd.option,
            })

            flightLineWaveGroup.add(startLightWaveMesh, endLightWaveMesh)
            flightLineWaveGroup.rotation.x = Math.PI
            flightLineGroup.add(flightLineMesh)

        });
        group.name = "flightLine_" + name
        group.add(flightLineGroup, flightLineWaveGroup)
        this.scene.add(group)

    }

}