// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(missing_docs)]
#![allow(unused_imports)]
#![allow(dead_code)]
#![allow(clippy::unusual_byte_groupings)]

//! Asahi File state

use crate::debug::*;
use crate::driver::AsahiDevice;
use crate::fw::types::*;
use crate::{alloc, buffer, driver, gem, gpu, mmu, render};
use kernel::drm::gem::BaseObject;
use kernel::prelude::*;
use kernel::sync::{smutex::Mutex, Ref};
use kernel::{bindings, drm};

const DEBUG_CLASS: DebugFlags = DebugFlags::File;

pub(crate) struct File {
    id: u64,
    vm: mmu::Vm,
    ualloc: Ref<Mutex<alloc::SimpleAllocator>>,
    ualloc_priv: Ref<Mutex<alloc::SimpleAllocator>>,
    ualloc_extra: alloc::SimpleAllocator,
    unk_page: GpuArray<u8>,
    renderer: Box<dyn render::Renderer>,
}

pub(crate) type DrmFile = drm::file::File<File>;

impl drm::file::DriverFile for File {
    type Driver = driver::AsahiDriver;

    fn open(device: &AsahiDevice) -> Result<Box<Self>> {
        debug::update_debug_flags();

        mod_dev_dbg!(device, "DRM device opened");
        let gpu = &device.data().gpu;
        let vm = gpu.new_vm()?;
        let id = gpu.ids().file.next();
        let ualloc = Ref::try_new(Mutex::new(alloc::SimpleAllocator::new_with_range(
            device,
            &vm,
            0x60_00000000,
            0x60_ffffffff,
            mmu::PROT_GPU_SHARED_RW,
            buffer::PAGE_SIZE,
        )))?;
        let ualloc_priv = Ref::try_new(Mutex::new(alloc::SimpleAllocator::new_with_range(
            device,
            &vm,
            0x61_00000000,
            0x61_ffffffff,
            mmu::PROT_GPU_FW_SHARED_RW,
            buffer::PAGE_SIZE,
        )))?;
        let mut ualloc_extra = alloc::SimpleAllocator::new_with_range(
            device,
            &vm,
            0x6f_ffff8000,
            0x70_00000000,
            mmu::PROT_GPU_SHARED_RW,
            0x4000,
        );
        let unk_page: GpuArray<u8> = ualloc_extra.array_empty(1)?;
        let renderer = device
            .data()
            .gpu
            .new_renderer(ualloc.clone(), ualloc_priv.clone())?;

        mod_dev_dbg!(device, "[File {}]: Opened successfully", id);
        Ok(Box::try_new(Self {
            id,
            vm,
            ualloc,
            ualloc_priv,
            ualloc_extra,
            unk_page,
            renderer,
        })?)
    }
}

impl File {
    pub(crate) fn submit(
        device: &AsahiDevice,
        data: &mut bindings::drm_asahi_submit,
        file: &DrmFile,
    ) -> Result<u32> {
        debug::update_debug_flags();

        let gpu = &device.data().gpu;
        gpu.update_globals();

        let id = gpu.ids().submission.next();
        mod_dev_dbg!(
            device,
            "[File {}]: IOCTL: submit (submission ID: {})",
            file.inner().id,
            id
        );
        let inner = file.inner();
        let ret = inner.renderer.render(&inner.vm, &inner.ualloc, data, id);
        if let Err(e) = ret {
            dev_info!(
                device,
                "[File {}]: IOCTL: submit failed! (submission ID: {} err: {:?})",
                file.inner().id,
                id,
                e
            );
            Err(e)
        } else {
            Ok(0)
        }
    }

    pub(crate) fn wait(
        device: &AsahiDevice,
        _data: &mut bindings::drm_asahi_wait,
        file: &DrmFile,
    ) -> Result<u32> {
        mod_dev_dbg!(device, "[File {}]: IOCTL: wait", file.inner().id);
        Ok(0)
    }

    pub(crate) fn create_bo(
        device: &AsahiDevice,
        data: &mut bindings::drm_asahi_create_bo,
        file: &DrmFile,
    ) -> Result<u32> {
        mod_dev_dbg!(
            device,
            "[File {}]: IOCTL: create_bo size={:#x?}",
            file.inner().id,
            data.size
        );

        let mut bo = gem::new_object(device, data.size as usize, data.flags)?;

        if data.flags & bindings::ASAHI_BO_PIPELINE != 0 {
            let start = 0x11_00000000;
            let end = start + 0xffffffff;
            let iova = bo.map_into_range(
                &file.inner().vm,
                start,
                end,
                mmu::UAT_PGSZ as u64,
                mmu::PROT_GPU_SHARED_RW,
            )?;
            data.offset = iova as u64 - 0x11_00000000;
        } else {
            // DEBUG: use different VM ranges for different files
            let start = 0x20_00000000;
            let end = start + 0x3f_ffffffff;

            let iova = bo.map_into_range(
                &file.inner().vm,
                start,
                end,
                mmu::UAT_PGSZ as u64,
                mmu::PROT_GPU_SHARED_RW,
            )?;
            data.offset = iova as u64;
        }
        let handle = bo.gem.create_handle(file)?;
        data.handle = handle;

        mod_dev_dbg!(
            device,
            "[File {}]: IOCTL: create_bo size={:#x} offset={:#x?} handle={:#x?}",
            file.inner().id,
            data.size,
            data.offset,
            data.handle
        );

        Ok(0)
    }

    pub(crate) fn mmap_bo(
        device: &AsahiDevice,
        data: &mut bindings::drm_asahi_mmap_bo,
        file: &DrmFile,
    ) -> Result<u32> {
        mod_dev_dbg!(
            device,
            "[File {}]: IOCTL: mmap_bo handle={:#x?}",
            file.inner().id,
            data.handle
        );

        let bo = gem::Object::lookup_handle(file, data.handle)?;

        data.offset = bo.create_mmap_offset()?;
        Ok(0)
    }

    pub(crate) fn get_param(
        device: &AsahiDevice,
        _data: &mut bindings::drm_asahi_get_param,
        file: &DrmFile,
    ) -> Result<u32> {
        mod_dev_dbg!(device, "[File {}]: IOCTL: get_param", file.inner().id);
        Ok(0)
    }

    pub(crate) fn get_bo_offset(
        device: &AsahiDevice,
        data: &mut bindings::drm_asahi_get_bo_offset,
        file: &DrmFile,
    ) -> Result<u32> {
        mod_dev_dbg!(
            device,
            "[File {}]: IOCTL: get_bo_offset handle={:#x?}",
            file.inner().id,
            data.handle
        );

        let mut bo = gem::ObjectRef::new(gem::Object::lookup_handle(file, data.handle)?);

        let start = 0x20_00000000;
        let end = start + 0x3f_ffffffff;

        // This can race other threads. Only one will win the map and the
        // others will return EBUSY.
        let iova = bo.map_into_range(
            &file.inner().vm,
            start,
            end,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_GPU_SHARED_RW,
        );

        if let Some(iova) = bo.iova(file.inner().vm.id()) {
            // If we have a mapping, call it good.
            data.offset = iova as u64;
            mod_dev_dbg!(
                device,
                "[File {}]: IOCTL: get_bo_offset handle={:#x?} offset={:#x?}",
                file.inner().id,
                data.handle,
                iova
            );
            Ok(0)
        } else {
            // Otherwise return the error, or a generic one if something
            // went very wrong and we lost the mapping.
            dev_err!(
                device,
                "[File {}]: IOCTL: get_bo_offset failed",
                file.inner().id
            );
            iova?;
            Err(EIO)
        }
    }

    pub(crate) fn vm_id(&self) -> u64 {
        self.vm.id()
    }
}

impl Drop for File {
    fn drop(&mut self) {
        mod_pr_debug!("[File {}]: Closing...", self.id);
    }
}
