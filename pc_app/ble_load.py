"""Paired, connected BLE telemetry. No scanning, pairing, mode or bond changes."""
import asyncio
from uuid import UUID
from load_protocol import identity,confirm,stats_payload,SyncUnavailable,AmbiguousTarget

SERVICE=UUID('7db8c001-4863-4e45-a612-7c8b2aa10001')
CHARACTERISTIC=UUID('7db8c002-4863-4e45-a612-7c8b2aa10001')

class BleLoad:
    def __init__(self):
        self.loop=asyncio.new_event_loop()
        self.device=self.service=self.characteristic=None

    def disconnect(self):
        self.characteristic=None
        for resource in (self.service,self.device):
            if resource is not None:
                try:resource.close()
                except Exception:pass
        self.service=self.device=None

    def close(self):
        self.disconnect();self.loop.close()

    def execute(self,task,timeout=2):
        try:return self.loop.run_until_complete(asyncio.wait_for(task,timeout))
        except BaseException:
            self.disconnect();raise

    def probe(self):return self.execute(self._probe(),6 if self.characteristic is None else 2)
    def send(self,values,peer):return self.execute(self._send(values,peer))

    async def read_identity(self):
        from winrt.windows.devices.bluetooth import BluetoothCacheMode
        from winrt.windows.devices.bluetooth.genericattributeprofile import GattCommunicationStatus
        read=await self.characteristic.read_value_with_cache_mode_async(BluetoothCacheMode.UNCACHED)
        if read.status!=GattCommunicationStatus.SUCCESS:raise SyncUnavailable('蓝牙键盘未响应，正在自动重试')
        peer=identity(bytes(read.value))
        if peer['mode']!=1:raise SyncUnavailable('蓝牙键盘模式已变化，正在重新连接')
        return peer

    async def _probe(self):
        from winrt.windows.devices.bluetooth import BluetoothLEDevice,BluetoothConnectionStatus,BluetoothCacheMode
        from winrt.windows.devices.enumeration import DeviceInformation
        from winrt.windows.devices.bluetooth.genericattributeprofile import GattCommunicationStatus
        # Enumeration is local Windows connection state, never a BLE radio scan.
        selector=BluetoothLEDevice.get_device_selector_from_connection_status(BluetoothConnectionStatus.CONNECTED)
        infos=await DeviceInformation.find_all_async_aqs_filter(selector)
        candidates=[info for info in infos if info.name=='LX Keyboard Dev']
        if len(candidates)>1:raise AmbiguousTarget('检测到多台蓝牙键盘，请仅连接要同步的一台')
        if not candidates:self.disconnect();return None
        if self.device and (self.device.device_id!=candidates[0].id or self.device.connection_status!=BluetoothConnectionStatus.CONNECTED):
            self.disconnect()
        if self.characteristic is None:
            self.device=await BluetoothLEDevice.from_id_async(candidates[0].id)
            if self.device is None:raise SyncUnavailable('蓝牙键盘暂不可访问，正在自动重试')
            if not self.device.device_information.pairing.is_paired:
                raise SyncUnavailable('请先在 Windows 中配对蓝牙键盘')
            result=await self.device.get_gatt_services_for_uuid_with_cache_mode_async(SERVICE,BluetoothCacheMode.CACHED)
            if result.status==GattCommunicationStatus.SUCCESS and not result.services:
                result=await self.device.get_gatt_services_for_uuid_with_cache_mode_async(SERVICE,BluetoothCacheMode.UNCACHED)
            if result.status!=GattCommunicationStatus.SUCCESS or len(result.services)!=1:
                for service in result.services:service.close()
                raise SyncUnavailable('蓝牙同步接口不可用，请检查连接或更新键盘固件')
            self.service=result.services[0]
            result=await self.service.get_characteristics_for_uuid_with_cache_mode_async(CHARACTERISTIC,BluetoothCacheMode.CACHED)
            if result.status!=GattCommunicationStatus.SUCCESS or len(result.characteristics)!=1:
                raise SyncUnavailable('蓝牙同步接口不可用，请更新键盘固件')
            self.characteristic=result.characteristics[0]
        return await self.read_identity()

    async def _send(self,values,peer):
        from winrt.windows.devices.bluetooth.genericattributeprofile import GattCommunicationStatus,GattWriteOption
        from winrt.windows.storage.streams import Buffer
        if self.characteristic is None:raise SyncUnavailable('蓝牙连接已变化，正在自动重试')
        payload=stats_payload(values,peer);buffer=Buffer(len(payload));buffer.length=len(payload)
        with memoryview(buffer) as view:view[:]=payload
        result=await self.characteristic.write_value_with_result_and_option_async(buffer,GattWriteOption.WRITE_WITH_RESPONSE)
        if result.status!=GattCommunicationStatus.SUCCESS:
            raise SyncUnavailable('蓝牙键盘未确认负载，请检查连接或更新键盘固件')
        if await self.read_identity()!=peer:raise SyncUnavailable('键盘身份已变化，暂缓同步')
        return peer
