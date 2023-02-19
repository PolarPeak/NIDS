import * as THREE from 'three'
const OrbitControls = require('three-orbit-controls')(THREE)
import { Line2 } from 'three/examples/jsm/lines/Line2'
import { LineGeometry } from 'three/examples/jsm/lines/LineGeometry'
import { LineMaterial } from 'three/examples/jsm/lines/LineMaterial'

import { LensflareElement, Lensflare } from 'three/examples/jsm/objects/Lensflare';


export default class ThreeEarth {
    constructor({ domElement, option, border }) {
        this.domElement = domElement
        this.option = option
        this.border = border
        this.scene
        this.camera
        this.light = {
            ambientLight: null,
            directionalLight: null,
        }
        this.renderer

        this.animationFrameId = null

        /**=================================================================================================*/
        this.textureLoader = new THREE.TextureLoader();



        this.earthCloudCover
        this.earthGroup = new THREE.Group()
        this.starsParticleSystem
        this.BeamLightWaveMesh = []
        this.FlightLineWaveMesh = []


        this.gridHelperName = "网格辅助"
        this.axesHelperName = "坐标辅助"
        this.lightBeamScatterSpeed = 0.007
    }
    init() {
        this.initScene()
        this.initCamera()
        this.initLight()
        this.initRenderer()
        this.initControls()
        this.initGridHelper()
        this.render()

        /**=================================================================================================*/
        this.initEarth()



        this.initSunshine()
        this.initStarrySkyBackground()

        this.scene.add(this.earthGroup)



        return this
    }
    /**
     * 初始化场景
     */
    initScene() {
        this.scene = new THREE.Scene()
        this.scene.fog = new THREE.Fog(0x020924, 200, 1000);
    }
    /**
     * 初始化相机
     */
    initCamera() {
        this.camera = new THREE.PerspectiveCamera(45, this.domElement.offsetWidth / this.domElement.offsetHeight, 0.1, 10000)
        this.camera.position.set(10, 10, 10)
        this.camera.lookAt(0, 0, 0)
    }
    /**
     * 初始化灯光
     */
    initLight() {

        // 平行光
        const dTlo = this.option.light.directionalLight
        this.light.directionalLight = new THREE.DirectionalLight(dTlo.color, dTlo.intensity);
        this.light.directionalLight.position.set(dTlo.x, dTlo.y, dTlo.z);
        this.light.directionalLight.lookAt(0, 0, 0)
        this.initDirectionalLightHelper()
        // 环境光
        const aLo = this.option.light.ambientLight
        this.light.ambientLight = new THREE.AmbientLight(aLo.color, aLo.intensity)

        this.scene.add(this.light.ambientLight, this.light.directionalLight)
    }
    /**
     * 设置平行光
     * @param {*} option 
     */
    setDirectionalLight(option) {
        this.light.directionalLight.position.set(option.x, option.y, option.z);
        this.light.directionalLight.lookAt(0, 0, 0)
        this.light.directionalLight.color.set(option.color)
        this.light.directionalLight.intensity = option.intensity

        const directionalLightHelper = this.scene.getObjectByName("directionalLightHelper");
        if (directionalLightHelper) {
            this.scene.remove(directionalLightHelper)
        }

        if (option.helper.show) {
            this.initDirectionalLightHelper()
        }
    }
    /**
   * 初始化平行光辅助
   */
    initDirectionalLightHelper() {
        const dTlo = this.option.light.directionalLight
        if (dTlo.helper.show) {
            this.directionalLightHelper = new THREE.DirectionalLightHelper(this.light.directionalLight, dTlo.helper.size, dTlo.helper.color);
            this.directionalLightHelper.name = "directionalLightHelper"
            this.scene.add(this.directionalLightHelper);
        }
    }
    /**
    * 设置环境光
    * @param {*} option 
    */
    setAmbientLight(option) {
        this.light.ambientLight.color.set(option.color)
        this.light.ambientLight.intensity = option.intensity
    }
    /**
     * 初始化渲染器
     */
    initRenderer() {
        this.renderer = new THREE.WebGLRenderer({
            antialias: true,
            alpha: true
        })
        this.renderer.setSize(this.domElement.offsetWidth, this.domElement.offsetHeight)
        this.renderer.setClearAlpha(0)
        this.domElement.appendChild(this.renderer.domElement)
        console.log("当前THREE版本号：", THREE.REVISION)
    }
    /**
     * 初始化控制器
     */
    initControls() {
        this.controls = new OrbitControls(this.camera, this.renderer.domElement);
        // 使动画循环使用时阻尼或自转 意思是否有惯性
        // this.controls.enableDamping = true;
        // 动态阻尼系数 就是鼠标拖拽旋转灵敏度
        // this.controls.dampingFactor = 0.2;
        // 是否可以缩放
        this.controls.enableZoom = true;
        // 是否自动旋转
        this.controls.autoRotate = false;
        this.controls.autoRotateSpeed = 2;
        // 设置相机距离原点的最远距离
        this.controls.minDistance = 5;
        // 设置相机距离原点的最远距离
        this.controls.maxDistance = 100;
        // 是否开启右键拖拽
        this.controls.enablePan = true;
        this.controls.enableKeys = false;
        this.controls.update()
    }
    /**
     * | 初始化网格辅助
     */
    initGridHelper(gridHelper) {
        if (gridHelper) {
            this.option.gridHelper = gridHelper
        }
        if (this.option.gridHelper.show) this.openGridHelper()
        if (!this.option.gridHelper.show) this.closeGridHelper()
    }
    /**
     * | 开启网格辅助
     */
    openGridHelper() {
        if (this.option.gridHelper.show) {
            const gh_width = this.option.gridHelper.width || 500
            const gh_height = this.option.gridHelper.height || 500
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
    /**
     * | 关闭网格辅助
     */
    closeGridHelper() {
        const gridHelper = this.scene.getObjectByName(this.gridHelperName);
        if (gridHelper) {
            this.scene.remove(gridHelper)
        }
    }
    /**
     * 初始化坐标辅助
     * @param {*} axesHelper 
     */
    initAxesHelper(axesHelper) {
        if (axesHelper) {
            this.option.axesHelper = axesHelper
        }
        if (this.option.axesHelper.show) this.openAxesHelper()
        if (!this.option.axesHelper.show) this.closeAxesHelper()
    }
    /**
     * 开启坐标辅助
     */
    openAxesHelper() {
        if (this.option.axesHelper.show) {
            const ah_size = this.option.axesHelper.size || 100
            const axesHelper = new THREE.AxesHelper(ah_size)
            axesHelper.name = this.axesHelperName
            this.closeAxesHelper()
            this.scene.add(axesHelper)
        }
    }
    /**
     * 关闭坐标辅助
     */
    closeAxesHelper() {
        const axesHelper = this.scene.getObjectByName(this.axesHelperName);
        if (axesHelper) {
            this.scene.remove(axesHelper)
        }
    }
    /**
     * 重置大小
     */
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
     * 渲染
     */
    render() {
        if (this.starsParticleSystem) {
            this.starsParticleSystem.rotation.y = this.starsParticleSystem.rotation.y + .0001;
        }
        if (this.earthGroup) {
            this.earthGroup.rotation.y = this.earthGroup.rotation.y + this.option.earth.speed * 0.0001
        }
        if (this.earthCloudCover) {
            this.earthCloudCover.rotation.y = this.earthCloudCover.rotation.y + 0.0002;
        }
        if (this.BeamLightWaveMesh.length) {

            this.BeamLightWaveMesh.forEach((mesh) => {
                mesh._s += this.lightBeamScatterSpeed;

                mesh.scale.set(mesh.size * mesh._s, mesh.size * mesh._s, mesh.size * mesh._s);
                if (mesh._s <= 1.5) {
                    mesh.material.opacity = (mesh._s - 1) * 2;
                } else if (mesh._s > 1.5 && mesh._s <= 2) {
                    mesh.material.opacity = 1 - (mesh._s - 1.5) * 2;
                } else {
                    mesh._s = 1.0;
                }
            });
        }
        if (this.FlightLineWaveMesh.length) {

            this.FlightLineWaveMesh.forEach((mesh) => {
                mesh._s += 0.007;

                mesh.scale.set(mesh.size * mesh._s, mesh.size * mesh._s, mesh.size * mesh._s);
                if (mesh._s <= 1.5) {
                    mesh.material.opacity = (mesh._s - 1) * 2;
                } else if (mesh._s > 1.5 && mesh._s <= 2) {
                    mesh.material.opacity = 1 - (mesh._s - 1.5) * 2;
                } else {
                    mesh._s = 1.0;
                }
            });
        }
        this.animationFrameId = requestAnimationFrame(this.render.bind(this));
        this.renderer.clear();
        this.renderer.render(this.scene, this.camera);
    }
    /**
     * 销毁实例
     */
    dispose() {
        try {
            cancelAnimationFrame(this.animationFrameId);
            this.camera = null;
            this.scene.traverse((child) => {
                if (child.material) child.material.dispose();
                if (child.geometry) child.geometry.dispose();
                child = null;
            });
            this.domElement.innerHTML = "";
            this.renderer.forceContextLoss();
            this.renderer.dispose();
            this.scene.clear();
            this.scene = null;
            this.camera = null;
            this.controls = null;
            this.renderer.domElement = null;
            this.renderer = null;
            console.log("clearScene");
        } catch (error) {
            console.log("ERROR ：clearScene");
        }
    }
    /**=================================================================================================*/
    /**
     * 初始化地球
     */
    initEarth() {
        const earth = this.earthGroup.getObjectByName("Earth");
        if (earth) this.earthGroup.remove(earth)

        const earthGeometry = new THREE.SphereGeometry(this.option.earth.radius * 0.1, this.option.earth.subdivision, this.option.earth.subdivision)

        var earthMaterial = null

        if (this.option.earth.textureShow) {
            const texture = this.textureLoader.load(this.option.earth.texture)
            earthMaterial = new THREE.MeshStandardMaterial({
                map: texture,
                side: THREE.DoubleSide,
                transparent: true,
                opacity: this.option.earth.opacity,
                wireframe: this.option.earth.wireframe,

            })
        } else {
            earthMaterial = new THREE.MeshStandardMaterial({
                color: this.option.earth.color,
                side: THREE.DoubleSide,
                transparent: true,
                opacity: this.option.earth.opacity,
                wireframe: this.option.earth.wireframe,
            })
        }

        const earthMesh = new THREE.Mesh(earthGeometry, earthMaterial)
        earthMesh.name = "Earth"
        this.earthGroup.add(earthMesh)
        this.earthGroup.rotation.set(0, 3.6, 0);

        this.initEarthBorders()

        console.log("初始化地球", this.earthGroup.children)
    }
    /**
     * 边框描边
     */
    initEarthBorders() {
        this.option.borders.forEach((item) => {
            this.initGeoJsonMapData(this.border[item.key], {
                show: item.show,
                key: item.key,
                line: {
                    color: item.color,
                    lineWidth: item.lineWidth,
                    opacity: item.opacity,
                },
            });
        });
        this.initEarthAperture()
    }
    /**
     * 初始化地球光圈
     */
    initEarthAperture() {
        const aperture = this.scene.getObjectByName("earthApertureSprite");
        if (aperture) this.scene.remove(aperture)

        if (this.option.aperture.show) {
            const texture = this.textureLoader.load(this.option.aperture.texture)
            const earthApertureMaterial = new THREE.SpriteMaterial({
                map: texture,
                transparent: true,
                color: this.option.aperture.color || 'red',
                opacity: this.option.aperture.opacity,
                depthWrite: false
            });
            const earthApertureSprite = new THREE.Sprite(earthApertureMaterial);
            earthApertureSprite.scale.set(this.option.earth.radius * 0.1 * 3, this.option.earth.radius * 0.1 * 3, 1);
            earthApertureSprite.name = "earthApertureSprite"
            this.scene.add(earthApertureSprite);
        }

        this.initEarthCloudCover()
    }
    /**
     * 初始化地球云层
     */
    initEarthCloudCover() {
        const cloud = this.earthGroup.getObjectByName("CloudCover");
        if (cloud) this.earthGroup.remove(cloud)

        if (this.option.cloud.show) {
            const texture = this.textureLoader.load(this.option.cloud.texture)
            const earthCloudCoverGeometry = new THREE.SphereGeometry(this.option.earth.radius * 0.1 + 0.1, 100, 100)
            const earthCloudCoverMaterial = new THREE.MeshStandardMaterial({
                map: texture,
                transparent: true,
                opacity: this.option.cloud.opacity,
            })
            this.earthCloudCover = new THREE.Mesh(earthCloudCoverGeometry, earthCloudCoverMaterial)
            this.earthCloudCover.name = "CloudCover"
            this.earthGroup.add(this.earthCloudCover)
        }
    }
    /**
     * 初始化太阳光
     */
    initSunshine() {
        const sunshine = this.light.directionalLight.getObjectByName("Sunshine");
        if (sunshine) this.light.directionalLight.remove(sunshine)
        if (this.option.sunshine.show) {
            const texture = this.textureLoader.load(this.option.sunshine.texture)
            const lensFlare = new Lensflare();
            lensFlare.addElement(new LensflareElement(texture, this.option.sunshine.size, 0));
            lensFlare.name = "Sunshine"
            this.light.directionalLight.add(lensFlare)
        }
    }
    /**
     * 初始化星空背景
     */
    initStarrySkyBackground() {
        const starsParticleSystem = this.scene.getObjectByName("starsParticleSystem");
        if (starsParticleSystem) this.scene.remove(starsParticleSystem)
        if (this.option.starrysky.show) {
            const texture = this.textureLoader.load(this.option.starrysky.texture)
            const randomPositions = [];
            const randomColors = [];
            const starsGeometry = new THREE.BufferGeometry();
            for (var i = 0; i < this.option.starrysky.number; i++) {
                // 随机坐标
                const vertex = new THREE.Vector3();
                vertex.x = Math.random() * 2 - 1;
                vertex.y = Math.random() * 2 - 1;
                vertex.z = Math.random() * 2 - 1;
                randomPositions.push(vertex.x, vertex.y, vertex.z);
                // 随机颜色
                const color = new THREE.Color();
                color.setHSL(Math.random() * 0.2 + 0.5, 0.55, Math.random() * 0.25 + 0.55);
                randomColors.push(color.r, color.g, color.b);
            }

            starsGeometry.setAttribute('position', new THREE.Float32BufferAttribute(randomPositions, 3));
            starsGeometry.setAttribute('color', new THREE.Float32BufferAttribute(randomColors, 3));
            const starsMaterial = new THREE.PointsMaterial({
                map: texture,
                size: 1,
                transparent: true,
                opacity: 1,
                vertexColors: true,
                blending: THREE.AdditiveBlending,
                sizeAttenuation: true
            });
            this.starsParticleSystem = new THREE.Points(starsGeometry, starsMaterial);
            this.starsParticleSystem.scale.set(300, 300, 300);
            this.starsParticleSystem.name = "starsParticleSystem"
            this.scene.add(this.starsParticleSystem)
        }

    }
    /**
     * 初始化geojson地图数据
     */
    initGeoJsonMapData(mapData, option) {
        const mapBorder = this.earthGroup.getObjectByName(option.key);
        if (mapBorder) this.earthGroup.remove(mapBorder)

        if (option.show) {
            const mapGroup = new THREE.Group()
            mapGroup.name = option.key
            mapData.features.forEach(item => {
                const areas = item.geometry.coordinates

                const areaGroup = new THREE.Group()
                areaGroup.name = `区域组-${item.properties.name}`

                const lineGroup = new THREE.Group()
                lineGroup.name = '描边线段组'
                const plateGroup = new THREE.Group()
                plateGroup.name = '地图板块组'

                areas.forEach((area) => {
                    var io = false, os = []
                    area.forEach((print, index) => {
                        if (print[index] instanceof Array) {
                            const px = print.map(pi => { return this.lglt2xyz(...pi, 0) })
                            lineGroup.add(this.drawMapStrokeV2(px, option.line))
                            // plateGroup.add(this.drawMapAreaMesh(px))

                        } else {
                            io = true, os.push(this.lglt2xyz(...print, 0))
                        }
                    })
                    if (io) {
                        lineGroup.add(this.drawMapStrokeV2(os, option.line))
                        // plateGroup.add(this.drawMapAreaMesh(os))
                    }
                })
                areaGroup.add(lineGroup, plateGroup)
                mapGroup.add(areaGroup)
            })
            this.earthGroup.add(mapGroup)
        }

    }
    /**
     * 绘制地图区域
     * @param {*} points 
     * @returns 
     */
    drawMapAreaMesh(points) {
        const shape = new THREE.Shape()
        shape.setFromPoints(points)
        const areaGeometry = new THREE.ExtrudeGeometry(shape, {
            depth: 0.5,
            bevelEnabled: false
        })
        const areaMateria = new THREE.MeshPhongMaterial({
            color: '#ffffff',
            transparent: true,
            opacity: 1
        })
        return new THREE.Mesh(areaGeometry, areaMateria)
    }
    /**
     * 绘制地图描边线段
     * @param {*} lines 
     * @returns 
     */
    drawMapStroke(lines) {
        const lineData = [];
        lines.forEach(({ x, y, z }) => lineData.push(x, y, z))
        const geometry = new THREE.BufferGeometry()
        geometry.setAttribute('position', new THREE.Float32BufferAttribute(lineData, 3));
        const lineMaterial = new THREE.LineDashedMaterial({
            transparent: true,
            color: "#fe6d9d",
            opacity: 1
        })
        return new THREE.Line(geometry, lineMaterial)
    }
    /**
     * 绘制地图描边线段V2
     * @param {*} lines 
     * @param {*} option 
     * @returns 
     */
    drawMapStrokeV2(lines, option) {
        const lineData = [];
        lines.forEach(({ x, y, z }) => lineData.push(x, y, z))
        const geometry = new LineGeometry()
        geometry.setPositions(lineData)
        const lineMaterial = new LineMaterial({
            transparent: true,
            color: option.color || "#fe6d9d",
            linewidth: (option.lineWidth * 0.001) || 0.0015,
            opacity: option.opacity || 1
        })
        return new Line2(geometry, lineMaterial)
    }
    /**
   * 清除组件
   * @param {*} name 
   */
    clearWidget(name) {
        const lightBeamScatter = this.earthGroup.getObjectByName("lightBeamScatter_" + name);
        if (lightBeamScatter) {
            this.earthGroup.remove(lightBeamScatter)
        }
        const flightLine = this.earthGroup.getObjectByName("flightLine_" + name);
        if (flightLine) {
            this.earthGroup.remove(flightLine)
        }
    }
    /**
     * 初始化光柱
     */
    initLightBeamScatter(data, name, option) {
        const lightBeamScatter = this.earthGroup.getObjectByName("lightBeamScatter_" + name);
        if (lightBeamScatter) {
            this.earthGroup.remove(lightBeamScatter)
        }
        const lightGroup = new THREE.Group();

        const r = option.radius * 0.01


        const lightColumnTexture = new THREE.TextureLoader().load(option.material.lightColumnTexture)
        const calloutTexture = new THREE.TextureLoader().load(option.material.calloutTexture)
        const calloutApertureTexture = new THREE.TextureLoader().load(option.material.calloutApertureTexture);



        this.lightBeamScatterSpeed = option.speed
        const material_color = option.material.color || 'red'
        const material_opacity = option.material.opacity || 1


        data.forEach(item => {
            const [x, y, z] = this.lglt2xyz(item.value[0], item.value[1], 0.1)
            const group = new THREE.Group();

            function createPointMesh(radius) {
                const geometry = new THREE.PlaneGeometry(radius * (r + 0.03), radius * option.baseHeight)
                geometry.rotateX(Math.PI / 2);//光柱高度方向旋转到z轴上
                geometry.translate(0, 0, radius * option.baseHeight * 0.5 / 2);//平移使光柱底部与XOY平面重合
                const beamLightMaterial = new THREE.MeshBasicMaterial({
                    map: lightColumnTexture,
                    color: material_color,
                    opacity: material_opacity,
                    transparent: true, //使用背景透明的png贴图，注意开启透明计算
                    side: THREE.DoubleSide, //双面可见
                    depthWrite: false, //是否对深度缓冲区有任何的影响
                })
                return new THREE.Mesh(geometry, beamLightMaterial)
            }

            function createLightWaveMesh(radius) {
                var geometry = new THREE.PlaneBufferGeometry(1, 1); //默认在XOY平面上
                geometry.rotateX(Math.PI);//光柱高度方向旋转到z轴上
                geometry.translate(0, 0, radius * 0.1 / 2);

                var material = new THREE.MeshBasicMaterial({
                    color: material_color,
                    opacity: material_opacity,
                    map: calloutTexture,
                    transparent: true, // 使用背景透明的png贴图，注意开启透明计算
                    side: THREE.DoubleSide, // 双面可见
                    depthWrite: false, // 禁止写入深度缓冲区数据
                });
                var mesh = new THREE.Mesh(geometry, material);
                var size = radius * r;//矩形平面Mesh的尺寸
                mesh.scale.set(size, size, size);//设置mesh大小
                return mesh;
            }

            function createWaveMesh(radius) {
                var geometry = new THREE.PlaneBufferGeometry(1, 1); //默认在XOY平面上
                geometry.rotateX(Math.PI);//光柱高度方向旋转到z轴上
                geometry.translate(0, 0, radius * 0.1 / 2);
                var material = new THREE.MeshBasicMaterial({
                    color: material_color,
                    opacity: material_opacity,
                    map: calloutApertureTexture,
                    transparent: true, //使用背景透明的png贴图，注意开启透明计算
                    side: THREE.DoubleSide, //双面可见
                    depthWrite: false, //禁止写入深度缓冲区数据
                });
                var mesh = new THREE.Mesh(geometry, material);
                var size = radius * r + 0.02;//矩形平面Mesh的尺寸
                mesh.scale.set(size, size, size);   //设置mesh大小
                mesh.size = size;
                mesh._s = Math.random() * 1.0 + 1.0;    //自定义属性._s表示mesh在原始大小基础上放大倍数  光圈在原来mesh.size基础上1~2倍之间变化

                return mesh;
            }

            const lightWaveMesh = createLightWaveMesh(this.option.earth.radius * 0.1)
            const pointMesh = createPointMesh(this.option.earth.radius * 0.1)
            const waveMesh = createWaveMesh(this.option.earth.radius * 0.1)

            this.BeamLightWaveMesh.push(waveMesh);
            group.add(pointMesh, pointMesh.clone().rotateZ(Math.PI / 2), lightWaveMesh, waveMesh)

            group.position.set(x, y, z)

            const coordVec3 = new THREE.Vector3(x, y, z).normalize();
            const meshNormal = new THREE.Vector3(0, 0, 1);
            group.quaternion.setFromUnitVectors(meshNormal, coordVec3);
            lightGroup.add(group)
        })
        lightGroup.name = "lightBeamScatter_" + name
        this.earthGroup.add(lightGroup)
    }
    /**
     * 初始化飞行线
     * @param {*} data 
     */
    initFlightLine(data, name, option) {

        const flightLine = this.earthGroup.getObjectByName("flightLine_" + name);
        if (flightLine) {
            this.earthGroup.remove(flightLine)
        }

        const flightLineGroup = new THREE.Group()

        const sc = new THREE.TextureLoader().load(option.scatterStart.calloutTexture)
        const scat = new THREE.TextureLoader().load(option.scatterStart.calloutApertureTexture);

        const ec = new THREE.TextureLoader().load(option.scatterEnd.calloutTexture)
        const ecat = new THREE.TextureLoader().load(option.scatterEnd.calloutApertureTexture);

        data.forEach(item => {

            const sv = this.lglt2xyz(item.coords[0][0], item.coords[0][1], 0.1)
            const ev = this.lglt2xyz(item.coords[1][0], item.coords[1][1], 0.1)

            const createFlightLine = () => {
                const [v0, v1, v2, v3] = this.getBezierCurveVPositions(sv, ev)
                // 贝塞尔曲线
                const curve = new THREE.CubicBezierCurve3(v0, v1, v2, v3);
                const points = curve.getSpacedPoints(100)
                const positions = []
                for (var j = 0; j < points.length; j++) {
                    positions.push(points[j].x, points[j].y, points[j].z);
                }
                const flightLineGeometry = new LineGeometry();
                flightLineGeometry.setPositions(positions)
                const flightLineMaterial = new LineMaterial({
                    color: option.line.color,
                    linewidth: option.line.width * 0.001,
                    dashed: false,
                    transparent: true,
                    opacity: option.line.opacity
                });
                return new Line2(flightLineGeometry, flightLineMaterial)
            }

            flightLineGroup.add(createFlightLine())

            const createLightWaveMesh = (radius, mt, map) => {
                var geometry = new THREE.PlaneBufferGeometry(1, 1);
                geometry.rotateX(Math.PI);
                var material = new THREE.MeshBasicMaterial({
                    color: mt.color,
                    opacity: mt.opacity,
                    map: map,
                    transparent: true,
                    side: THREE.DoubleSide,
                    depthWrite: false,
                });
                var mesh = new THREE.Mesh(geometry, material);
                var size = radius * 0.002 * mt.size;
                mesh.scale.set(size, size, size);
                return mesh;
            }

            const createWaveMesh = (radius, mt, map) => {
                var geometry = new THREE.PlaneBufferGeometry(1, 1);
                geometry.rotateX(Math.PI);
                var material = new THREE.MeshBasicMaterial({
                    color: mt.color,
                    opacity: mt.opacity,
                    map: map,
                    transparent: true,
                    side: THREE.DoubleSide,
                    depthWrite: false,
                });
                var mesh = new THREE.Mesh(geometry, material);
                var size = radius * 0.003 * mt.size;
                mesh.scale.set(size, size, size);
                mesh.size = size;
                mesh._s = Math.random() * 1.0 + 1.0;
                return mesh;
            }

            const sGroup = new THREE.Group()
            const eGroup = new THREE.Group()

            const sLightWaveMesh = createLightWaveMesh(this.option.earth.radius, option.scatterStart, sc)
            const sWaveMesh = createWaveMesh(this.option.earth.radius, option.scatterStart, scat)
            sGroup.add(sLightWaveMesh, sWaveMesh)
            sGroup.position.set(sv.x, sv.y, sv.z)

            const eLightWaveMesh = createLightWaveMesh(this.option.earth.radius, option.scatterEnd, ec)
            const eWaveMesh = createWaveMesh(this.option.earth.radius, option.scatterEnd, ecat)
            eGroup.add(eLightWaveMesh, eWaveMesh)
            eGroup.position.set(ev.x, ev.y, ev.z)

            this.FlightLineWaveMesh.push(sWaveMesh, eWaveMesh)

            sGroup.quaternion.setFromUnitVectors(new THREE.Vector3(0, 0, 1), new THREE.Vector3(sv.x, sv.y, sv.z).normalize());
            eGroup.quaternion.setFromUnitVectors(new THREE.Vector3(0, 0, 1), new THREE.Vector3(ev.x, ev.y, ev.z).normalize());

            flightLineGroup.add(sGroup, eGroup)

        })
        flightLineGroup.name = "flightLine_" + name
        this.earthGroup.add(flightLineGroup)

    }
    /**
     * 计算贝塞尔曲线，控制点坐标信息
     * @param {*} v1 
     * @param {*} v2 
     */
    getBezierCurveVPositions(v0, v3) {

        // 夹角 0 ~ Math.PI
        const angle = v0.angleTo(v3) * 1.5 / Math.PI / 0.1;
        const aLen = angle * 0.4, hLen = angle * angle * 12;

        // 法线向量
        const rayLine = new THREE.Ray(new THREE.Vector3(0, 0, 0), v0.clone().add(v3.clone()).divideScalar(2));

        // 顶点坐标
        const vTop = rayLine.at(
            hLen / rayLine.at(1, new THREE.Vector3()).distanceTo(new THREE.Vector3(0, 0, 0)),
            new THREE.Vector3()
        );

        // 控制点坐标
        const v1 = v0.clone().lerp(vTop, aLen / v0.clone().distanceTo(vTop));
        const v2 = v3.clone().lerp(vTop, aLen / v3.clone().distanceTo(vTop));

        return [v0, v1, v2, v3]
    }
    /**
     * 经纬度转换xyz
     */
    lglt2xyz(lng, lat, offset) {
        const theta = (90 + lng) * (Math.PI / 180);
        const phi = (90 - lat) * (Math.PI / 180);

        offset = this.option.cloud.show ? offset : this.option.cloud.show
        return (new THREE.Vector3()).setFromSpherical(new THREE.Spherical(this.option.earth.radius * 0.1 + offset, phi, theta));
    }
}