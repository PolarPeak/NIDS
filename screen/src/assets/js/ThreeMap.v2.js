import * as THREE from 'three'
import ThreeMapV2Impl from "./ThreeMap.v2.impl"

export default class ThreeMap extends ThreeMapV2Impl {

    constructor({ domElement, mapName, mapData, option }) {

        super({ domElement, mapName, mapData, option })

        this.raycaster
        this.mouseVector2
        this.animationFrameId = null

        this.ambientLight

        this.directionalLight
        this.directionalLightHelper
    }
    init() {
        this.initScene()
        this.initCamera()
        this.initRenderer()
        this.initGridHelper()
        this.initAxesHelper()
        this.render()

        this.initLight()

        this.initMapByGeoJson(this.mapData)

        // 设置高亮显示 | 射线触碰
        if (this.option.highlightState) {
            this.domElement.addEventListener('mousemove', this.onMouseEvent.bind(this));
        } else {
            this.domElement.removeEventListener('mousemove', this.onMouseEvent.bind(this));
        }
        return this
    }
    /**
     *  渲染动画帧
     */
    render() {
        if (this.waveMeshArr.length) {

            this.waveMeshArr.forEach((mesh) => {
                mesh._s += this.lightBeamScatterSpeed;

                mesh.scale.set(mesh.size * mesh._s + 1, mesh.size * mesh._s + 1, mesh.size * mesh._s + 1);

                if (mesh._s <= 1.5) {
                    //mesh._s=1，透明度=0 mesh._s=1.5，透明度=1
                    mesh.material.opacity = (mesh._s - 1) * 2;
                } else if (mesh._s > 1.5 && mesh._s <= 2) {
                    //mesh._s=1.5，透明度=1 mesh._s=2，透明度=0
                    mesh.material.opacity = 1 - (mesh._s - 1.5) * 2;
                } else {
                    mesh._s = 1.0;
                }
            });
        }


        this.animationFrameId = requestAnimationFrame(this.render.bind(this));
        this.renderer.clear();
        this.renderer.render(this.scene, this.camera);

        if (this.flightLineTexture) this.flightLineTexture.offset.x -= this.flightLineSpeed;

    }
    /**
     * 初始化场景
     */
    initScene() {
        this.scene = new THREE.Scene();
    }
    /**
     * 初始化渲染器
     */
    initRenderer() {
        this.renderer = new THREE.WebGLRenderer({
            // 开启抗锯齿渲染
            antialias: true,
            alpha: true,
        })
        console.log("this.domElement", this.domElement)
        this.renderer.setSize(this.domElement.offsetWidth, this.domElement.offsetHeight);
        this.renderer.setClearAlpha(0);
        this.domElement.appendChild(this.renderer.domElement)
        console.log("THREE当前版本：", THREE.REVISION)
    }
    /**
     * 初始化相机
     */
    initCamera() {
        this.camera = new THREE.PerspectiveCamera(45, this.domElement.offsetWidth / this.domElement.offsetHeight, 0.1, 10000);
        this.camera.position.set(this.option.angleX, this.option.angleY, this.option.distance);
        this.camera.lookAt(0, 0, 0);
    }
    /**
     * 初始化灯光
     */
    initLight() {
        // 点光源
        // const pointLight = new THREE.PointLight(0xffffff);
        // pointLight.position.set(30, 20, 50);
        // this.scene.add(pointLight);
        // var pointLightHelper = new THREE.PointLightHelper(pointLight, 5, 0xff00ff);
        // this.scene.add(pointLightHelper);

        // 从正上方照射过来的白色平行光，0.5的光强。
        const dTlo = this.option.light.directionalLight
        this.directionalLight = new THREE.DirectionalLight(dTlo.color, dTlo.intensity);
        this.directionalLight.position.set(dTlo.x, dTlo.y, dTlo.z);
        this.directionalLight.lookAt(0, 0, 0)

        this.initDirectionalLightHelper()

        // 环境光
        const aLo = this.option.light.ambientLight
        this.ambientLight = new THREE.AmbientLight(aLo.color, aLo.intensity)


        this.scene.add(this.ambientLight, this.directionalLight);
    }
    /**
     * 初始化平行光辅助
     */
    initDirectionalLightHelper() {
        const dTlo = this.option.light.directionalLight
        if (dTlo.helper.show) {
            this.directionalLightHelper = new THREE.DirectionalLightHelper(this.directionalLight, dTlo.helper.size, dTlo.helper.color);
            this.directionalLightHelper.name = "directionalLightHelper"
            this.scene.add(this.directionalLightHelper);
        }
    }
    /**
     * 鼠标移动监听事件 | 用于触摸元素
     * @param {*} event 
     */
    onMouseEvent(event) {
        try {
            if (!this.mouseVector2) this.mouseVector2 = new THREE.Vector2();
            if (!this.raycaster) this.raycaster = new THREE.Raycaster();
            //通过鼠标点击的位置计算出raycaster所需要的点的位置，以屏幕中心为原点，值的范围为-1到1.
            this.mouseVector2.x = (event.clientX / window.innerWidth) * 2 - 1;
            this.mouseVector2.y = - (event.clientY / window.innerHeight) * 2 + 1;
            // 通过鼠标点的位置和当前相机的矩阵计算出raycaster
            this.raycaster.setFromCamera(this.mouseVector2, this.camera);
            // 获取raycaster直线和所有模型相交的数组集合
            const intersects = this.raycaster.intersectObjects(this.mapModel.children);

            // 恢复所有状态 材质状态 颜色 透明度 等等
            this.mapModel.children.forEach(area => {
                area.children.forEach(item => {
                    item.children.forEach(item2 => {
                        if (item2.name === '区域板块') {
                            item2.material.color.set(this.option.area.color || '#007cff');
                            item2.material.opacity = this.option.area.opacity || 0.5;
                        }
                        if (item2.name === '标签') {
                            item2.material.color.set(this.option.label.color || '#ffffff')
                            item2.material.opacity = this.option.label.opacity || 0.7
                        }
                    })
                })
            })
            //将所有的相交的模型的颜色设置为红色，如果只需要将第一个触发事件，那就数组的第一个模型改变颜色即可
            for (var i = 0; i < intersects.length; i++) {

                if (intersects[0].object.name === '区域板块') {
                    // 区域板块设置
                    intersects[0].object.material.color.set(this.option.hover.area.color || 0xff0000);
                    intersects[0].object.material.opacity = this.option.hover.area.opacity || 0.5
                    try {
                        // 找到板块中的组
                        const labelGroup = intersects[0].object.parent.parent.children.find(item => item.name == '标签文字组')
                        labelGroup.children[0].material.opacity = this.option.hover.label.opacity || 1
                        labelGroup.children[0].material.color.set(this.option.hover.label.color || '#ffffff')
                    } catch (error) {
                        console.warn("设置标签材质错误！！！")
                    }

                }
            }
        } catch (error) {
            console.warn("onMouseEvent", "抛出错误！！！")
            console.error(error)
        }
    }
    /**
     * 销毁
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
            this.raycaster = null;
            this.mouseVector2 = null;
            this.projection = null;
            console.log("clearScene");
        } catch (error) {
            console.log("ERROR ：clearScene");
        }

    }
}