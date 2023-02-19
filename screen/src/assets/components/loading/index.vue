<template>
  <transition name="fade">
    <div class="load-img-wrapper">
      <img class="load-img" :src="loadingImg" />
    </div>
  </transition>
</template>

<script>
export default {
  data() {
    return {
      loadingImg: require("@/assets/images/loading.gif"),
    };
  },
};
</script>

<style scoped>
.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.2s ease-in;
}
.fade-enter,
.fade-leave,
.fade-leave-to {
  opacity: 0.5;
}
.load-img-wrapper {
  width: 100%;
  height: 100%;
  background: #030303;
  display: flex;
  justify-content: center;
  align-items: center;
}
.load-img-wrapper .load-img {
  max-width: 200px;
  max-height: 200px;
  width: 100%;
}
</style>