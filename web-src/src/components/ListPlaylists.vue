<template>
  <div
    v-for="playlist in playlists"
    :key="playlist.itemId"
    class="media"
    :playlist="playlist"
    @click="open_playlist(playlist.item)"
  >
    <figure class="media-left fd-has-action">
      <span class="icon"
        ><mdicon :name="icon_name(playlist.item)" size="16"
      /></span>
    </figure>
    <div class="media-content fd-has-action is-clipped">
      <h1 class="title is-6" v-text="playlist.item.name" />
    </div>
    <div class="media-right">
      <a @click.prevent.stop="open_dialog(playlist.item)">
        <span class="icon has-text-dark"
          ><mdicon name="dots-vertical" size="16"
        /></span>
      </a>
    </div>
  </div>
  <teleport to="#app">
    <modal-dialog-playlist
      :show="show_details_modal"
      :playlist="selected_playlist"
      @close="show_details_modal = false"
    />
  </teleport>
</template>

<script>
import ModalDialogPlaylist from '@/components/ModalDialogPlaylist.vue'

export default {
  name: 'ListPlaylists',
  components: { ModalDialogPlaylist },

  props: ['playlists'],

  data() {
    return {
      show_details_modal: false,
      selected_playlist: {}
    }
  },

  methods: {
    open_playlist: function (playlist) {
      if (playlist.type !== 'folder') {
        this.$router.push({ path: '/playlists/' + playlist.id + '/tracks' })
      } else {
        this.$router.push({ path: '/playlists/' + playlist.id })
      }
    },

    open_dialog: function (playlist) {
      this.selected_playlist = playlist
      this.show_details_modal = true
    },

    icon_name: function (playlist) {
      if (playlist.type === 'folder') {
        return 'folder'
      } else if (playlist.type === 'rss') {
        return 'rss'
      } else {
        return 'music-box-multiple'
      }
    }
  }
}
</script>

<style></style>
