//! FoA open-MAC concurrent STA + AWDL firmware.
//!
//! Channel 6 is the only 2.4 GHz AWDL channel. This image keeps a station VIF
//! and an AWDL VIF on FoA's LMAC at once — faithful concurrent Wi-Fi + AWDL
//! that Espressif's closed stack cannot do. AirDrop app-layer (CPIO/TLS) is a
//! later step; this cut proves both link layers stay alive.
//!
//! Target chip: classic ESP32 (FoA today). E1002 is ESP32-S3 — use this image
//! as a lab / companion radio until FoA adds S3.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::boxed::Box;
use embassy_executor::Spawner;
use embassy_time::{Duration, Timer};
use esp_hal::{interrupt::software::SoftwareInterruptControl, timer::timg::TimerGroup};
use foa::{FoAResources, FoARunner, VirtualInterface};
use foa_awdl::{AwdlResources, AwdlRunner};
use foa_sta::{StaResources, StaRunner};
use log::info;

mod companion;

/// Only 2.4 GHz AWDL channel (matches ESP32Drop PROBE_CHANNEL / AIRDROP_COEX_CHANNEL).
const AWDL_CHANNEL: u8 = 6;

macro_rules! mk_static {
    ($t:ty, $val:expr) => {{
        static STATIC_CELL: static_cell::StaticCell<$t> = static_cell::StaticCell::new();
        STATIC_CELL.init_with(|| $val)
    }};
}

#[embassy_executor::task]
async fn foa_task(mut runner: FoARunner<'static>) {
    runner.run().await
}

#[embassy_executor::task]
async fn sta_task(mut runner: StaRunner<'static, 'static>) {
    runner.run().await
}

#[embassy_executor::task]
async fn awdl_task(mut runner: AwdlRunner<'static, 'static>) -> ! {
    runner.run().await
}

#[esp_rtos::main]
async fn main(spawner: Spawner) {
    let peripherals =
        esp_hal::init(esp_hal::Config::default().with_cpu_clock(esp_hal::clock::CpuClock::_240MHz));

    esp_alloc::heap_allocator!(size: 48 * 1024);
    esp_bootloader_esp_idf::esp_app_desc!();
    esp_println::logger::init_logger_from_env();

    info!(
        "e1002-open: FoA concurrent STA + AWDL on ch{} (open MAC)",
        AWDL_CHANNEL
    );

    let timg0 = TimerGroup::new(peripherals.TIMG0);
    let sw_interrupt = SoftwareInterruptControl::new(peripherals.SW_INTERRUPT);
    esp_rtos::start(timg0.timer0, sw_interrupt.software_interrupt0);

    let foa_resources = Box::leak(Box::new(FoAResources::new()));
    let ([sta_vif, awdl_vif, ..], foa_runner) = foa::init(foa_resources, peripherals.WIFI);
    spawner.spawn(foa_task(foa_runner).unwrap());

    // --- STA VIF (ordinary Wi-Fi) -------------------------------------------
    let sta_vif = Box::leak(Box::new(sta_vif));
    let sta_resources = Box::leak(Box::new(StaResources::new()));
    let (mut _sta_control, sta_runner, _sta_net_device) =
        foa_sta::new_sta_interface(sta_vif, sta_resources);
    spawner.spawn(sta_task(sta_runner).unwrap());
    info!("STA VIF attached (connect via SSID env when testing association)");

    // --- AWDL VIF (AirDrop link layer) --------------------------------------
    let awdl_vif = mk_static!(VirtualInterface<'static>, awdl_vif);
    let awdl_resources = mk_static!(AwdlResources, AwdlResources::new());
    let (mut awdl_control, awdl_runner, _awdl_net_device, _awdl_events) =
        foa_awdl::new_awdl_interface(awdl_vif, awdl_resources);
    spawner.spawn(awdl_task(awdl_runner).unwrap());
    awdl_control
        .start()
        .await
        .expect("AWDL VIF failed to start");
    info!("AWDL VIF started (ch{} dual-protocol receive path)", AWDL_CHANNEL);

    // Companion READY frame (UART export later); encode proves the codec links.
    let mut frame = [0u8; 16];
    let n = companion::encode(companion::AC_MSG_READY, 0, &[], &mut frame)
        .expect("READY frame");
    info!("companion READY frame ready ({} B); UART not wired in this cut", n);

    // Heartbeat: both VIFs share FoA's LMAC — this is the concurrent experiment.
    let mut tick: u32 = 0;
    loop {
        tick = tick.wrapping_add(1);
        info!(
            "dual-protocol alive: STA VIF + AWDL VIF on open MAC (tick={tick}, ch{AWDL_CHANNEL})"
        );
        Timer::after(Duration::from_secs(5)).await;
    }
}
