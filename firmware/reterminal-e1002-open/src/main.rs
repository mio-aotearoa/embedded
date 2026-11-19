//! FoA open-MAC concurrent STA + AWDL firmware.
//!
//! Runs `foa_sta` and `foa_awdl` together on FoA's LMAC (channel 6). Ordinary
//! Wi-Fi association/DHCP and AWDL peer/service discovery are real FoA paths.
//! Companion READY / SESSION_OPEN frames are transmitted on UART1 (GPIO17) for
//! an E1002 photoframe or host.
//!
//! Target: ESP32-S3 (E1002). Upstream FoA is patched in-tree for S3.
//!
//!   SSID=ch6ap PASSWORD=secret cargo build --release --features firmware,esp32s3 \
//!     --target xtensa-esp32s3-none-elf

#![no_std]
#![no_main]

extern crate alloc;

use esp_backtrace as _;

use alloc::collections::btree_map::BTreeMap;
use embassy_executor::Spawner;
use embassy_net::{DhcpConfig, Runner as NetRunner, StackResources as NetStackResources};
use embassy_sync::{blocking_mutex::raw::CriticalSectionRawMutex, channel::Channel};
use embassy_time::{Duration, Timer};
use embedded_io::Write;
use esp_hal::{
    interrupt::software::SoftwareInterruptControl,
    rng::Rng,
    timer::timg::TimerGroup,
    uart::{Config as UartConfig, Uart},
};
use foa::{FoAResources, FoARunner, VirtualInterface};
use foa_awdl::{AwdlEvent, AwdlEventQueueReceiver, AwdlResources, AwdlRunner};
use foa_sta::{ConnectionConfig, Credentials, StaNetDevice, StaResources, StaRunner};
use log::{info, warn};

use e1002_open::companion;

const AWDL_CHANNEL: u8 = 6;
const COMPANION_UART_BAUD: u32 = 115_200;

type CompChannel = Channel<CriticalSectionRawMutex, CompMsg, 8>;
static COMP_TX: CompChannel = Channel::new();

enum CompMsg {
    Ready,
    SessionOpen(u8),
}

macro_rules! mk_static {
    ($t:ty, $val:expr) => {{
        static STATIC_CELL: static_cell::StaticCell<$t> = static_cell::StaticCell::new();
        STATIC_CELL.init_with(|| $val)
    }};
}

fn wifi_ssid() -> Option<&'static str> {
    match option_env!("SSID") {
        Some(s) if !s.is_empty() => Some(s),
        _ => None,
    }
}

fn wifi_credentials() -> Option<Credentials<'static>> {
    option_env!("PASSWORD")
        .filter(|p| !p.is_empty())
        .map(Credentials::Passphrase)
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

#[embassy_executor::task]
async fn sta_net_task(mut runner: NetRunner<'static, StaNetDevice<'static>>) -> ! {
    runner.run().await
}

#[embassy_executor::task]
async fn companion_uart_task(mut uart: Uart<'static, esp_hal::Blocking>) {
    let mut seq: u8 = 0;
    let mut frame = [0u8; 64];
    loop {
        let msg = COMP_TX.receive().await;
        let (ty, payload): (u8, &[u8]) = match &msg {
            CompMsg::Ready => (companion::AC_MSG_READY, &[]),
            CompMsg::SessionOpen(id) => (companion::AC_MSG_SESSION_OPEN, core::slice::from_ref(id)),
        };
        let Some(n) = companion::encode(ty, seq, payload, &mut frame) else {
            warn!("companion encode overflow");
            continue;
        };
        seq = seq.wrapping_add(1);
        if let Err(e) = uart.write_all(&frame[..n]) {
            warn!("companion UART write failed: {e:?}");
        }
        let _ = uart.flush();
        info!("companion TX type={ty:#x} seq={} len={n}", seq.wrapping_sub(1));
    }
}

#[embassy_executor::task]
async fn awdl_event_task(rx: AwdlEventQueueReceiver<'static>) {
    let mut img_id: u8 = 1;
    loop {
        match rx.receive().await {
            AwdlEvent::PeerDiscovered(addr) => {
                info!(
                    "AWDL peer {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                    addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]
                );
            }
            AwdlEvent::PeerWentStale(p) => {
                info!(
                    "AWDL peer stale {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                    p.address[0],
                    p.address[1],
                    p.address[2],
                    p.address[3],
                    p.address[4],
                    p.address[5]
                );
            }
            AwdlEvent::DiscoveredServiceForPeer {
                address,
                airdrop_port: Some(port),
                airplay_port,
            } => {
                info!(
                    "AirDrop on {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} port={port} airplay={airplay_port:?}",
                    address[0], address[1], address[2], address[3], address[4], address[5]
                );
                let _ = COMP_TX.try_send(CompMsg::SessionOpen(img_id));
                img_id = img_id.wrapping_add(1);
            }
            AwdlEvent::DiscoveredServiceForPeer {
                address,
                airdrop_port: None,
                airplay_port,
            } => {
                info!(
                    "AWDL services {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} airplay={airplay_port:?}",
                    address[0], address[1], address[2], address[3], address[4], address[5]
                );
            }
        }
    }
}

#[esp_rtos::main]
async fn main(spawner: Spawner) {
    let peripherals =
        esp_hal::init(esp_hal::Config::default().with_cpu_clock(esp_hal::clock::CpuClock::_240MHz));

    esp_alloc::heap_allocator!(size: 48 * 1024);
    esp_bootloader_esp_idf::esp_app_desc!();
    esp_println::logger::init_logger_from_env();

    info!("e1002-open: FoA STA + AWDL concurrent on ch{AWDL_CHANNEL}");

    let timg0 = TimerGroup::new(peripherals.TIMG0);
    let sw_interrupt = SoftwareInterruptControl::new(peripherals.SW_INTERRUPT);
    esp_rtos::start(timg0.timer0, sw_interrupt.software_interrupt0);

    // Companion UART1 TX on GPIO17 (RX unused; photoframe/host listens).
    let uart = Uart::new(
        peripherals.UART1,
        UartConfig::default().with_baudrate(COMPANION_UART_BAUD),
    )
    .expect("UART1")
    .with_tx(peripherals.GPIO17);
    spawner.spawn(companion_uart_task(uart).unwrap());
    COMP_TX.send(CompMsg::Ready).await;
    info!("companion UART1 TX GPIO17 @ {COMPANION_UART_BAUD} baud");

    let foa_resources = mk_static!(FoAResources, FoAResources::new());
    let ([sta_vif, awdl_vif, ..], foa_runner) = foa::init(foa_resources, peripherals.WIFI);
    spawner.spawn(foa_task(foa_runner).unwrap());

    let sta_resources = mk_static!(StaResources<'static>, StaResources::default());
    let (mut sta_control, sta_runner, sta_net_device) = foa_sta::new_sta_interface(
        mk_static!(VirtualInterface<'static>, sta_vif),
        sta_resources,
    );
    spawner.spawn(sta_task(sta_runner).unwrap());
    let mac = sta_control.randomize_mac_address().expect("STA MAC");
    info!("STA MAC {mac:02x?}");

    let awdl_resources = mk_static!(AwdlResources, AwdlResources::new());
    let (mut awdl_control, awdl_runner, _awdl_net, awdl_events) = foa_awdl::new_awdl_interface(
        mk_static!(VirtualInterface<'static>, awdl_vif),
        awdl_resources,
    );
    spawner.spawn(awdl_task(awdl_runner).unwrap());
    awdl_control.randomize_mac_address();
    awdl_control.start().await.expect("AWDL start");
    info!(
        "AWDL listening ch{AWDL_CHANNEL} ipv6={}",
        awdl_control.own_ipv6_addr()
    );
    spawner.spawn(awdl_event_task(awdl_events).unwrap());

    if let Some(ssid) = wifi_ssid() {
        info!("STA connecting SSID={ssid} (use a 2.4 GHz ch{AWDL_CHANNEL} AP)");
        let net_resources = mk_static!(NetStackResources<3>, NetStackResources::new());
        let (net_stack, net_runner) = embassy_net::new(
            sta_net_device,
            embassy_net::Config::dhcpv4(DhcpConfig::default()),
            net_resources,
            Rng::new().random() as u64,
        );
        sta_control
            .connect_by_ssid(
                ssid,
                Some(ConnectionConfig {
                    beacon_timeout: None,
                    ..Default::default()
                }),
                wifi_credentials(),
            )
            .await
            .expect("STA connect");
        spawner.spawn(sta_net_task(net_runner).unwrap());
        net_stack.wait_config_up().await;
        if let Some(cfg) = net_stack.config_v4() {
            info!("STA DHCP {}", cfg.address);
        }
        loop {
            info!(
                "dual-protocol live: STA associated + AWDL peers on ch{AWDL_CHANNEL}"
            );
            Timer::after(Duration::from_secs(30)).await;
        }
    } else {
        warn!("SSID unset — scanning on STA while AWDL runs (set SSID=/PASSWORD= to associate)");
        let mut found = BTreeMap::new();
        let _ = sta_control
            .scan_continuously(None, &mut found, |bss| {
                info!(
                    "STA BSS \"{}\" ch={} rssi={} (AWDL also on ch{AWDL_CHANNEL})",
                    bss.ssid, bss.channel, bss.last_rssi
                );
                true
            })
            .await;
    }
}
