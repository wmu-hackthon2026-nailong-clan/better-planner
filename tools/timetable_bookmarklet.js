// ============================================================================
// 教务系统课表抓取书签脚本（bookmarklet）——源码版
// ----------------------------------------------------------------------------
// 用途：在正方教务系统「课表查询」页面（已登录、已过验证码）点击该书签，
//       自动调用后台接口抓取完整课表 JSON（含 xsxx / kbList / sjkList），
//       并触发浏览器下载为 .json 文件，供「更好规划」应用「导入课表」使用。
//
// 适用：温州医科大学教学综合信息服务平台（zftal，_path=/jwglxt），
//       以及其他走同一套 xskbcxMobile_cxXsgrkb 接口的正方教务系统。
//
// 原理：页面隐藏字段已含学年(xnm_hide)/学期(xqm_hide)/周数(zs_hide)。
//       脚本用「原生 DOM + XMLHttpRequest」读取这些值并发请求（不依赖
//       jQuery/Zepto，避免页面库未加载或全局被覆盖导致 jQuery is not
//       defined 的问题），拿到原始 data 后拼成完整 JSON 下载。
//       无需重新登录、无需绕验证码——因为你本来就在已登录的课表页里。
//
// 下载方式：直接用「不插入 DOM 的临时 <a>」触发 a.click() 下载。
//       经验证：不 appendChild 到 body、不延迟 revokeObjectURL，反而
//       能规避部分浏览器对 bookmarklet 触发下载的静默拦截。
// ============================================================================

(function () {
  'use strict';

  function notify(m, e) {
    try {
      if (window.layer && layer.msg) {
        layer.msg(m, { time: 4000 });
        return;
      }
    } catch (x) { /* ignore */ }
    alert(m);
  }

  function val(id) {
    var el = document.getElementById(id);
    return el ? (el.value || '').trim() : '';
  }

  try {
    var path = window._path || '/jwglxt';
    var xnm = val('xnm_hide');
    var xqm = val('xqm_hide');
    var zs = val('zs_hide');
    var kblx = window.kblx_str || '1';

    if (!xnm || !xqm) {
      notify('未找到学年/学期信息，请确认在「课表查询」页面再点书签。', true);
      return;
    }

    var params = 'xnm=' + encodeURIComponent(xnm) +
                 '&xqm=' + encodeURIComponent(xqm) +
                 '&kblx=' + encodeURIComponent(kblx) +
                 '&doType=app';
    if (kblx === '1') {
      params += '&zs=' + encodeURIComponent(zs || '1');
    }

    var xhr = new XMLHttpRequest();
    xhr.open('POST', path + '/kbcx/xskbcxMobile_cxXsgrkb.html', false);
    xhr.setRequestHeader('Content-Type',
                         'application/x-www-form-urlencoded; charset=UTF-8');
    xhr.send(params);

    var data = null;
    if (xhr.status >= 200 && xhr.status < 300) {
      try {
        data = JSON.parse(xhr.responseText);
      } catch (e) {
        notify('返回的不是有效 JSON：' + e.message, true);
        return;
      }
    } else {
      notify('接口调用失败（HTTP ' + xhr.status + '）。请确认已登录且会话未过期。', true);
      return;
    }

    if (!data || !data.kbList || data.kbList.length === 0) {
      notify('未获取到课表数据（kbList 为空），可能当前没有排课。', true);
      return;
    }

    var payload = {
      qsxqj: data.qsxqj || '',
      xsxx: data.xsxx || {},
      kbList: data.kbList || [],
      sjkList: data.sjkList || []
    };

    var xnStart = parseInt(xnm, 10) || 0;
    var fname = '课表_' + (xnStart ? xnStart + '-' + (xnStart + 1) : xnm) +
                '_第' + xqm + '学期';
    if (data.xsxx && data.xsxx.XM) fname += '_' + data.xsxx.XM;
    fname += '.json';

    var blob = new Blob([JSON.stringify(payload, null, 2)],
                        { type: 'application/json' });
    var a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = fname;
    a.click();

    notify('已抓取 ' + data.kbList.length + ' 条课表，下载「' + fname +
           '」后到应用点「导入课表」选择它。');
  } catch (e) {
    notify('脚本执行出错：' + e.message, true);
  }
})();
