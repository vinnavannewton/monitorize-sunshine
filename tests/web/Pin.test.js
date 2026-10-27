import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

const apiFetch = vi.hoisted(() => vi.fn())
vi.mock('../../src_assets/common/assets/web/fetch_utils', () => ({ apiFetch }))
vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({ default: { template: '<div />' } }))
import Pin from '../../src_assets/common/assets/web/Pin.vue'

const wrappers = []

/** Mount the actual upstream pairing page with instance-local pending requests. */
async function pairingPage(pairings) {
  apiFetch.mockImplementation(async (_url, options) => ({
    ok: true,
    json: async () => options.method === 'GET'
      ? { pairings }
      : { status: true, pairing_complete: true },
  }))
  const wrapper = mount(Pin, {
    global: {
      mocks: { $t: key => key },
      provide: { i18n: { t: key => key } },
    },
  })
  wrappers.push(wrapper)
  await flushPromises()
  return wrapper
}

afterEach(() => {
  for (const wrapper of wrappers.splice(0)) wrapper.unmount()
  vi.clearAllMocks()
})

describe('instance-local pending pairing selection', () => {
  it('selects one request automatically and submits its explicit ID', async () => {
    const id = 'a1'.repeat(16)
    const wrapper = await pairingPage([{ id, name: 'Tablet', address: '192.0.2.10' }])
    expect(wrapper.get('#pairing-input').element.value).toBe(id)
    await wrapper.get('#pin-input').setValue('0123')
    await wrapper.get('#name-input').setValue('My tablet')
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    const submit = apiFetch.mock.calls.find(([, options]) => options.method === 'POST')
    expect(submit[0]).toBe('./api/pin')
    expect(JSON.parse(submit[1].body)).toEqual({ pairing_id: id, pin: '0123', name: 'My tablet' })
    expect(wrapper.get('[role="alert"]').text()).toBe('pin.pair_success')
  })

  it('requires an explicit choice when multiple clients are pending', async () => {
    const first = 'a1'.repeat(16)
    const second = 'b2'.repeat(16)
    const wrapper = await pairingPage([{ id: first, name: 'First' }, { id: second, name: 'Second' }])
    await wrapper.get('form').trigger('submit')
    expect(apiFetch.mock.calls.some(([, options]) => options.method === 'POST')).toBe(false)
    expect(wrapper.get('[role="alert"]').text()).toBe('pin.select_pairing_required')
    await wrapper.get('#pairing-input').setValue(second)
    await wrapper.get('#pin-input').setValue('4567')
    await wrapper.get('#name-input').setValue('Second')
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    const [, options] = apiFetch.mock.calls.find(([, options]) => options.method === 'POST')
    expect(JSON.parse(options.body).pairing_id).toBe(second)
  })

  it('cancels only the selected request', async () => {
    const id = 'c3'.repeat(16)
    const wrapper = await pairingPage([{ id, name: 'Tablet' }])
    await wrapper.get('button[type="button"]').trigger('click')
    await flushPromises()
    const [url, options] = apiFetch.mock.calls.find(([, options]) => options.method === 'DELETE')
    expect(url).toBe('./api/pin')
    expect(JSON.parse(options.body)).toEqual({ pairing_id: id })
  })
})
