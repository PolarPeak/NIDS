import Vue from 'vue'
import App from './App.vue'
import _ from 'lodash'
import * as Scroll from "./assets/js/seamscroll.min";

Vue.config.productionTip = false
Vue.prototype._ = _
Vue.prototype.Scroll = Scroll

new Vue({
  render: h => h(App),
}).$mount('#app')
