// 空的 service worker：不做離線快取，只是讓瀏覽器判定這個網站符合
// 「可安裝為 App」的條件，加到主畫面後才會用 standalone 模式打開
// （沒有網址列、沒有瀏覽器操作列），跟原生 app 一樣。
self.addEventListener("fetch", () => {});
