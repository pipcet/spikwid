use ash::{version::DeviceV1_0, vk};
use smallvec::SmallVec;
use std::sync::Arc;

use crate::{command::CommandBuffer, conv, Backend, RawDevice};
use hal::{command, pool};

#[derive(Debug)]
pub struct RawCommandPool {
    pub(crate) raw: vk::CommandPool,
    pub(crate) device: Arc<RawDevice>,
}

impl pool::CommandPool<Backend> for RawCommandPool {
    unsafe fn reset(&mut self, release_resources: bool) {
        let flags = if release_resources {
            vk::CommandPoolResetFlags::RELEASE_RESOURCES
        } else {
            vk::CommandPoolResetFlags::empty()
        };

        assert_eq!(Ok(()), self.device.raw.reset_command_pool(self.raw, flags));
    }

    unsafe fn allocate<E>(&mut self, num: usize, level: command::Level, list: &mut E)
    where
        E: Extend<CommandBuffer>,
    {
        let info = vk::CommandBufferAllocateInfo::builder()
            .command_pool(self.raw)
            .level(conv::map_command_buffer_level(level))
            .command_buffer_count(num as u32);

        let device = &self.device;

        list.extend(
            device
                .raw
                .allocate_command_buffers(&info)
                .expect("Error on command buffer allocation")
                .into_iter()
                .map(|buffer| CommandBuffer {
                    raw: buffer,
                    device: Arc::clone(device),
                }),
        );
    }

    unsafe fn free<I>(&mut self, cbufs: I)
    where
        I: Iterator<Item = CommandBuffer>,
    {
        let buffers: SmallVec<[vk::CommandBuffer; 16]> = cbufs.map(|buffer| buffer.raw).collect();
        self.device.raw.free_command_buffers(self.raw, &buffers);
    }
}
