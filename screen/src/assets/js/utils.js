/**
 * 深度Copy
 * @param {*} obj
 * @returns
 */
export const deepCopy = (obj) => {
  let type = Object.prototype.toString.call(obj);
  if (type == "[object Array]") {
    let backObj = [];
    for (let val of obj) {
      backObj.push(deepCopy(val));
    }
    return backObj;
  }
  if (type == "[object Object]") {
    let backObj = {};
    for (let key in obj) {
      if (obj.hasOwnProperty(key)) {
        backObj[key] = deepCopy(obj[key]);
      }
    }
    return backObj;
  }
  return obj;
};
/**
 * rgba转16进制
 */
export const rgbaToHexColor = (color) => {
  var values = color
    .replace(/rgba?\(/, "")
    .replace(/\)/, "")
    .replace(/[\s+]/g, "")
    .split(",");
  var a = parseFloat(values[3] || 1),
    r = Math.floor(a * parseInt(values[0]) + (1 - a) * 255),
    g = Math.floor(a * parseInt(values[1]) + (1 - a) * 255),
    b = Math.floor(a * parseInt(values[2]) + (1 - a) * 255);
  return (
    "#" +
    ("0" + r.toString(16)).slice(-2) +
    ("0" + g.toString(16)).slice(-2) +
    ("0" + b.toString(16)).slice(-2)
  );
};
/**
 * 改变颜色中rgba格式的透明度
 * @param {*} color 
 * @param {*} a 
 * @returns 
 */
export const rgbaChangeA = (color, a) => {
  return color.substring(0, color.lastIndexOf(",")) + ", " + a + ")";
};
/**
 * 获取数组中字段所有不重复的枚举值
 * @param {*} data 
 * @param {*} key 
 * @returns 
 */
export const getArrayValueEnum = (data, key) => {
  let arr = [];
  data.forEach(item => {
    arr.indexOf(item[key]) == -1 ? arr.push(item[key]) : "";
  })
  return arr;
}

export default {
  /**
   * 获取数组中字段所有不重复的枚举值
   * @param {*} data 
   * @param {*} key 
   * @returns 
   */
  getArrayValueEnum: function (data, key) {
    let arr = [];
    data.forEach(item => {
      arr.indexOf(item[key]) == -1 ? arr.push(item[key]) : "";
    })
    return arr;
  },
  rgbaChangeA: function (color, a) {
    return color.substring(0, color.lastIndexOf(",")) + ", " + a + ")";
  },
  deepCopy: function (obj) {
    let type = Object.prototype.toString.call(obj);
    if (type == "[object Array]") {
      let backObj = [];
      for (let val of obj) {
        backObj.push(deepCopy(val));
      }
      return backObj;
    }
    if (type == "[object Object]") {
      let backObj = {};
      for (let key in obj) {
        if (obj.hasOwnProperty(key)) {
          backObj[key] = deepCopy(obj[key]);
        }
      }
      return backObj;
    }
    return obj;
  },
  getArraySum : (data, key) => {
    let sum = 0;
    data.forEach((item) => {
      sum += item[key];
    });
    return sum;
  },
  getArrayValueEnum : (data, key) => {
    let arr = [];
    data.forEach(item => {
      arr.indexOf(item[key]) == -1 ? arr.push(item[key]) : "";
    })
    return arr;
  }
}