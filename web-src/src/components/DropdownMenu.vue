<template>
  <div
    v-click-away="onClickOutside"
    class="dropdown"
    :class="{ 'is-active': is_active }"
  >
    <div class="dropdown-trigger">
      <button
        class="button"
        aria-haspopup="true"
        aria-controls="dropdown-menu"
        @click="is_active = !is_active"
      >
        <span v-text="option.name" />
        <span class="icon"><mdicon name="chevron-down" size="16" /></span>
      </button>
    </div>
    <div id="dropdown-menu" class="dropdown-menu" role="menu">
      <div class="dropdown-content">
        <a
          v-for="o in options"
          :key="o.id"
          class="dropdown-item"
          :class="{ 'is-active': modelValue === o.id }"
          @click="select(o)"
          v-text="o.name"
        />
      </div>
    </div>
  </div>
</template>

<script>
export default {
  name: 'DropdownMenu',
  props: ['modelValue', 'options'],
  emits: ['update:modelValue'],

  data() {
    return {
      is_active: false
    }
  },

  computed: {
    option: {
      get() {
        return this.options.find((option) => option.id === this.modelValue)
      }
    }
  },

  methods: {
    onClickOutside(event) {
      this.is_active = false
    },

    select(option) {
      this.is_active = false
      this.$emit('update:modelValue', option.id)
    }
  }
}
</script>

<style></style>
