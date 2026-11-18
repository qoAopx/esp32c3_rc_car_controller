
"use strict";

// ========================================
// BLE設定
// ESP32-C3側と同じUUIDにすること
// ========================================

const SERVICE_UUID =
    "12345678-1234-5678-1234-56789abcdef0";

const CHARACTERISTIC_UUID =
    "12345678-1234-5678-1234-56789abcdef1";

// 送信周期（ms）
const SEND_INTERVAL = 40;


// ========================================
// BLE状態
// ========================================

let device = null;
let characteristic = null;

let connected = false;
let sending = false;


// ========================================
// 操作値
// ========================================

// 左右 -100 ～ +100
let leftRight = 0;

// 前後 -100 ～ +100
let forwardBack = 0;


// ========================================
// HTML要素
// ========================================

const statusText =
    document.getElementById("status");

const statusLamp =
    document.getElementById("statusLamp");

const connectBtn =
    document.getElementById("connectBtn");


// ========================================
// ステータス表示
// ========================================

function updateStatus(text, state) {

    statusText.textContent = text;

    statusLamp.className =
        "lamp " + state;
}


// ========================================
// 接続ボタン表示
// ========================================

function updateButton(isConnected) {

    connectBtn.textContent =
        isConnected ? "BLE 切断" : "BLE 接続";

    connectBtn.classList.toggle(
        "disconnect",
        isConnected
    );
}


// ========================================
// BLE接続
// ========================================

connectBtn.addEventListener("click", async () => {

    // 接続済みなら切断
    if (connected) {
        disconnectBLE();
        return;
    }

    // Web Bluetooth確認
    if (!navigator.bluetooth) {

        updateStatus(
            "BLE非対応ブラウザ",
            "error"
        );

        return;
    }

    connectBtn.disabled = true;

    updateStatus(
        "接続中...",
        "connecting"
    );

    try {

        // ESP32-C3を検索
        device = await navigator.bluetooth.requestDevice({

            filters: [
                {
                    services: [SERVICE_UUID]
                }
            ]

        });

        // 切断イベント登録
        device.addEventListener(
            "gattserverdisconnected",
            onDisconnected
        );

        // GATT接続
        const server =
            await device.gatt.connect();

        // サービス取得
        const service =
            await server.getPrimaryService(
                SERVICE_UUID
            );

        // キャラクタリスティック取得
        characteristic =
            await service.getCharacteristic(
                CHARACTERISTIC_UUID
            );

        connected = true;

        updateStatus(
            "BLE 接続済み",
            "connected"
        );

        updateButton(true);

    } catch (error) {

        console.error(
            "BLE接続エラー:",
            error
        );

        updateStatus(
            "接続失敗",
            "error"
        );

        if (device?.gatt?.connected) {
            device.gatt.disconnect();
        }

        connected = false;
        characteristic = null;

        updateButton(false);

    } finally {

        connectBtn.disabled = false;

    }

});


// ========================================
// BLE切断
// ========================================

function disconnectBLE() {

    if (device && device.gatt.connected) {

        updateStatus(
            "切断中...",
            "connecting"
        );

        device.gatt.disconnect();

    } else {

        onDisconnected();

    }

}


// ========================================
// 切断イベント
// ========================================

function onDisconnected() {

    connected = false;

    characteristic = null;

    sending = false;

    // 操作値をゼロにする
    leftRight = 0;
    forwardBack = 0;

    resetSticks();

    updateStatus(
        "未接続",
        "disconnected"
    );

    updateButton(false);

}


// ========================================
// BLE送信
//
// 2バイト形式
//
// Byte 0 : 左右 -100 ～ +100
// Byte 1 : 前後 -100 ～ +100
//
// 符号付き8bit整数
// ========================================

async function sendData() {

    if (
        !connected ||
        !characteristic ||
        sending
    ) {
        return;
    }

    sending = true;

    try {

        const bytes = new Uint8Array([

            leftRight & 0xFF,

            forwardBack & 0xFF

        ]);

        // Write Without Response優先
        if (
            characteristic.properties.writeWithoutResponse
        ) {

            await characteristic.writeValueWithoutResponse(
                bytes
            );

        } else {

            await characteristic.writeValueWithResponse(
                bytes
            );

        }

    } catch (error) {

        console.error(
            "BLE送信エラー:",
            error
        );

    } finally {

        sending = false;

    }

}


// ========================================
// 定期送信
// 40ms = 25Hz
// ========================================

setInterval(
    sendData,
    SEND_INTERVAL
);


// ========================================
// スティック初期化
//
// axis = "y" : 前後
// axis = "x" : 左右
// ========================================

function setupStick(stickId, knobId, axis) {

    const stick =
        document.getElementById(stickId);

    const knob =
        document.getElementById(knobId);

    let pointerId = null;


    // ------------------------------------
    // スティック移動
    // ------------------------------------

    function move(event) {

        const rect =
            stick.getBoundingClientRect();

        const cx =
            rect.left + rect.width / 2;

        const cy =
            rect.top + rect.height / 2;

        const dx =
            event.clientX - cx;

        const dy =
            event.clientY - cy;

        // 操作可能半径
        const radius =
            rect.width * 0.32;

        // 前後または左右を取得
        let value =
            axis === "x" ? dx : -dy;

        // 範囲制限
        value = Math.max(
            -radius,
            Math.min(radius, value)
        );

        // -100 ～ +100に変換
        const percent = Math.round(
            value / radius * 100
        );

        // ノブ移動位置
        const px =
            axis === "x" ? value : 0;

        const py =
            axis === "y" ? -value : 0;

        knob.style.left =
            `calc(50% + ${px}px)`;

        knob.style.top =
            `calc(50% + ${py}px)`;


        // 左右操作
        if (axis === "x") {

            leftRight = percent;

            document.getElementById(
                "steeringValue"
            ).textContent =
                "LR : " + percent;

        }

        // 前後操作
        else {

            forwardBack = percent;

            document.getElementById(
                "throttleValue"
            ).textContent =
                "FB : " + percent;

        }

    }


    // ------------------------------------
    // スティックを中央に戻す
    // ------------------------------------

    function reset() {

        knob.style.left = "50%";
        knob.style.top = "50%";

        if (axis === "x") {

            leftRight = 0;

            document.getElementById(
                "steeringValue"
            ).textContent = "LR : 0";

        } else {

            forwardBack = 0;

            document.getElementById(
                "throttleValue"
            ).textContent = "FB : 0";

        }

        sendData();

    }


    // ------------------------------------
    // タッチ開始
    // ------------------------------------

    stick.addEventListener(
        "pointerdown",
        event => {

            if (pointerId !== null) return;

            pointerId = event.pointerId;

            stick.setPointerCapture(
                pointerId
            );

            move(event);

        }
    );


    // ------------------------------------
    // タッチ移動
    // ------------------------------------

    stick.addEventListener(
        "pointermove",
        event => {

            if (
                event.pointerId !== pointerId
            ) {
                return;
            }

            move(event);

        }
    );


    // ------------------------------------
    // タッチ終了
    // ------------------------------------

    function release(event) {

        if (
            event.pointerId !== pointerId
        ) {
            return;
        }

        pointerId = null;

        reset();

    }


    stick.addEventListener(
        "pointerup",
        release
    );

    stick.addEventListener(
        "pointercancel",
        release
    );


    // ------------------------------------
    // キャプチャ解除
    // ------------------------------------

    stick.addEventListener(
        "lostpointercapture",
        event => {

            if (
                event.pointerId === pointerId
            ) {

                pointerId = null;

                reset();

            }

        }
    );

}


// ========================================
// スティック初期化
// ========================================

function resetSticks() {

    document.getElementById(
        "throttleKnob"
    ).style.left = "50%";

    document.getElementById(
        "throttleKnob"
    ).style.top = "50%";

    document.getElementById(
        "steeringKnob"
    ).style.left = "50%";

    document.getElementById(
        "steeringKnob"
    ).style.top = "50%";

    document.getElementById(
        "throttleValue"
    ).textContent = "FB : 0";

    document.getElementById(
        "steeringValue"
    ).textContent = "LR : 0";

}


// ========================================
// 操作軸の割り当て
// ========================================

// 左スティック：前後
setupStick(
    "throttleStick",
    "throttleKnob",
    "y"
);

// 右スティック：左右
setupStick(
    "steeringStick",
    "steeringKnob",
    "x"
);


// ========================================
// ページが非表示になった場合
// ========================================

document.addEventListener(
    "visibilitychange",
    () => {

        if (document.hidden) {

            leftRight = 0;
            forwardBack = 0;

            resetSticks();

            sendData();

        }

    }
);
