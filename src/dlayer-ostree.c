/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <ostree.h>
#include <json-glib/json-glib.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "libglnx.h"

struct DlayerOstree;

typedef struct {
  const char *name;
  gboolean (*fn) (struct DlayerOstree *self, int argc, char **argv, GCancellable *cancellable, GError **error);
} Subcommand;

#define SUBCOMMANDPROTO(name) static gboolean dlayer_ostree_builtin_ ## name (struct DlayerOstree *self, int argc, char **argv, GCancellable *cancellable, GError **error)

SUBCOMMANDPROTO(importone);
SUBCOMMANDPROTO(checkout);

static Subcommand commands[] = {
  { "importone", dlayer_ostree_builtin_importone },
  { "checkout", dlayer_ostree_builtin_checkout },
  { NULL, NULL }
};

static gboolean opt_version;
static char *opt_repo;

static GOptionEntry global_entries[] = {
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information and exit", NULL },
  { "repo", 0, 0, G_OPTION_ARG_STRING, &opt_repo, "Path to OSTree repository", "PATH" },
  { NULL }
};

static char *opt_branch;

static GOptionEntry importone_options[] = {
  { "branch", 0, 0, G_OPTION_ARG_STRING, &opt_branch, "Update this branch to point to the layer", "PATH" },
  { NULL }
};

static gboolean opt_user_mode;

static GOptionEntry checkout_options[] = {
  { "user-mode", 'U', 0, G_OPTION_ARG_NONE, &opt_user_mode, "Do not change file ownership or initialize extended attributes", NULL },
  { NULL }
};

static gboolean
option_context_parse (GOptionContext *context,
                      const GOptionEntry *main_entries,
                      int *argc,
                      char ***argv,
                      GCancellable *cancellable,
                      GError **error)
{
  gboolean ret = FALSE;

  if (main_entries != NULL)
    g_option_context_add_main_entries (context, main_entries, NULL);

  g_option_context_add_main_entries (context, global_entries, NULL);

  if (!g_option_context_parse (context, argc, argv, error))
    goto out;

  if (opt_version)
    {
      g_print ("%s\n  +default\n", PACKAGE_STRING);
      exit (EXIT_SUCCESS);
    }

  ret = TRUE;
out:
  return ret;
}

struct DlayerOstree {
  OstreeRepo *repo;
};

static char *
branch_name_for_docker_id (const char *layerid)
{
  return g_strconcat ("dockerimg/", layerid, NULL);
}

static gboolean
common_init (struct DlayerOstree *self,
             GError **error)
{
  gboolean ret = FALSE;
  
  if (!opt_repo)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "--repo must be specified");
      goto out;
    }

  { g_autoptr(GFile) repopath = g_file_new_for_path (opt_repo);
    self->repo = ostree_repo_new (repopath);
    if (!ostree_repo_open (self->repo, NULL, error))
      goto out;
  }
  
  ret = TRUE;
 out:
  return ret;
}

static gboolean
create_empty_default_dir (OstreeRepo  *repo,
                          OstreeMutableTree *mtree,
                          GCancellable *cancellable,
                          GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) dirmeta = NULL;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autofree guchar *tmp_csum = NULL;
  char checksum[65];

  file_info = g_file_info_new ();
  g_file_info_set_attribute_uint32 (file_info, "unix::uid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::gid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::mode", 0755 | S_IFDIR);

  dirmeta = ostree_create_directory_metadata (file_info, NULL);

  if (!ostree_repo_write_metadata (repo, OSTREE_OBJECT_TYPE_DIR_META, NULL,
                                   dirmeta, &tmp_csum, cancellable, error))
    goto out;

  ostree_checksum_inplace_from_bytes (tmp_csum, checksum);
  ostree_mutable_tree_set_metadata_checksum (mtree, checksum);

  ret = TRUE;
 out:
  return ret;
}

static gboolean
dlayer_ostree_builtin_importone (struct DlayerOstree *self, int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *optcontext;
  const char *layerjson;
  const char *layerid;
  const char *tarball = "-";
  g_autofree char *layer_string = NULL;
  g_autofree char *branch = NULL;
  g_autofree char *commit_checksum = NULL;
  glnx_unref_object JsonParser *parser = json_parser_new ();
  glnx_unref_object OstreeMutableTree *mtree = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GVariant) layer_variant = NULL;
  g_autoptr(GVariantDict) layer_variant_dict = NULL;
  g_autoptr(GVariant) metav = NULL;

  optcontext = g_option_context_new ("LAYERJSON TARBALL - Import a Docker image layer");

  if (!option_context_parse (optcontext, importone_options, &argc, &argv,
                             cancellable, error))
    goto out;

  if (argc < 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "A LAYERID argument is required");
      goto out;
    }
  else if (argc > 3)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Too many arguments");
      goto out;
    }

  if (!common_init (self, error))
    goto out;

  layerjson = argv[1];
  if (argc == 2)
    tarball = "/dev/fd/0";
  else
    {
      g_assert (argc == 3);
      tarball = argv[2];
    }

  if (!g_file_get_contents (layerjson, &layer_string, NULL, error))
    goto out;

  if (!json_parser_load_from_data (parser, layer_string, -1, error))
    goto out;

  layer_variant = g_variant_ref_sink (json_gvariant_deserialize (json_parser_get_root (parser), NULL, error));
  if (!layer_variant)
    {
      g_prefix_error (error, "Converting layer JSON to GVariant: "); 
      goto out;
    }
  if (!g_variant_is_of_type (layer_variant, (GVariantType*)"a{sv}"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid non-object layer JSON");
      goto out;
    }

  layer_variant_dict = g_variant_dict_new (layer_variant);
  if (!g_variant_dict_lookup (layer_variant_dict, "id", "&s", &layerid))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Missing required key 'id'");
      goto out;
    }

  branch = branch_name_for_docker_id (layerid);

  { g_autoptr(GVariantBuilder) metabuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (metabuilder, "{sv}", "docker.layer", g_variant_new_string (layer_string));
    metav = g_variant_ref_sink (g_variant_builder_end (metabuilder));
  }

  mtree = ostree_mutable_tree_new ();

  if (!ostree_repo_prepare_transaction (self->repo, NULL, cancellable, error))
    goto out;

  { g_autoptr(GFile) stdin_file = g_file_new_for_path (tarball);
    if (!ostree_repo_write_archive_to_mtree (self->repo, stdin_file, mtree,
                                             NULL, TRUE,
                                             cancellable, error))
    goto out;
  }

  if (!ostree_mutable_tree_get_metadata_checksum (mtree))
    {
      if (!create_empty_default_dir (self->repo, mtree, cancellable, error))
        goto out;
    }

  if (!ostree_repo_write_mtree (self->repo, mtree, &root, cancellable, error))
    goto out;

  { /* TODO: Parse the layerid creation timestamp */

    if (!ostree_repo_write_commit (self->repo, NULL, "", NULL, metav,
                                   OSTREE_REPO_FILE (root),
                                   &commit_checksum, cancellable, error))
      goto out;
  }

  ostree_repo_transaction_set_ref (self->repo, NULL, branch, commit_checksum);

  if (opt_branch)
    ostree_repo_transaction_set_ref (self->repo, NULL, opt_branch, commit_checksum);

  if (!ostree_repo_commit_transaction (self->repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
resolve_layers (struct DlayerOstree *self,
                const char          *layerid,
                guint                recursion,
                GPtrArray           *layer_ids,
                GCancellable        *cancellable,
                GError             **error)
{
  gboolean ret = FALSE;
  g_autofree char *rev = NULL;
  g_autoptr(GVariant) commit = NULL;
  g_autoptr(GVariant) commitmeta = NULL;
  g_autoptr(GVariantDict) commitmeta_vdict = NULL;
  glnx_unref_object JsonParser *parser = json_parser_new ();
  JsonNode *layer_root;
  JsonObject *layer_root_o;
  JsonNode *layer_parent;
  const char *layer_string;
  g_autoptr(GVariant) layerv = NULL;
  g_autoptr(GVariantDict) layer_vdict = NULL;
  const guint maxlayers = 1024; /* Arbitrary */

  if (recursion >= maxlayers)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Layer maximum %u exceeded", maxlayers);
      goto out;
    }

  if (g_cancellable_set_error_if_cancelled (cancellable,  error))
    goto out;

  if (!ostree_repo_resolve_rev (self->repo, layerid, FALSE, &rev, error))
    goto out;

  if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_COMMIT, rev,
                                 &commit, error))
    goto out;

  commitmeta = g_variant_get_child_value (commit, 0);
  commitmeta_vdict = g_variant_dict_new (commitmeta);
  if (!g_variant_dict_lookup (commitmeta_vdict, "docker.layer", "&s", &layer_string, error))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Missing required key 'docker.layer'");
      goto out;
    }

  if (!json_parser_load_from_data (parser, layer_string, -1, error))
    goto out;

  layer_root = json_parser_get_root (parser);
  if (json_node_get_node_type (layer_root) != JSON_NODE_OBJECT)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid 'docker.layer'");
      goto out;
    }
  layer_root_o = json_node_get_object (layer_root);

  layer_parent = json_object_get_member (layer_root_o, "parent");
  if (layer_parent)
    {
      const char *parent = json_node_get_string (layer_parent);
      g_autofree char *branch = NULL;

      if (!parent)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Invalid 'docker.layer'");
          goto out;
        }

      branch = branch_name_for_docker_id (parent);

      if (!resolve_layers (self, branch, recursion + 1, layer_ids, cancellable, error))
        goto out;
    }
  else
    {
      g_ptr_array_add (layer_ids, g_steal_pointer (&rev));
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
dlayer_ostree_builtin_checkout (struct DlayerOstree *self, int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *optcontext;
  const char *layerid;
  const char *destination;
  g_autoptr(GPtrArray) layer_commits = g_ptr_array_new_with_free_func (g_free);
  OstreeRepoCheckoutOptions ocheckout_options = { 0, };
  glnx_fd_close int target_dfd = -1;
  guint i;
  
  optcontext = g_option_context_new ("LAYERID DESTINATION - Check out a Docker layer (with all of its parents)");

  if (!option_context_parse (optcontext, checkout_options, &argc, &argv,
                             cancellable, error))
    goto out;

  if (argc < 3)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "LAYERID and DESTINATION are required");
      goto out;
    }
  else if (argc > 3)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Too many arguments");
      goto out;
    }

  if (!common_init (self, error))
    goto out;

  layerid = argv[1];
  destination = argv[2];

  if (!resolve_layers (self, layerid, 0, layer_commits, cancellable, error))
    goto out;

  g_assert (layer_commits->len > 0);
  
  ocheckout_options.overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES;
  ocheckout_options.process_whiteouts = TRUE;

  if (opt_user_mode)
    ocheckout_options.mode = OSTREE_REPO_CHECKOUT_MODE_USER;

  /* Check out the root, then open it */
  if (!ostree_repo_checkout_tree_at (self->repo, &ocheckout_options,
                                     AT_FDCWD, destination,
                                     layer_commits->pdata[0],
                                     cancellable, error))
    goto out;

  if (!glnx_opendirat (AT_FDCWD, destination, TRUE, &target_dfd, error))
    goto out;

  /* Now check out parent layers */
  for (i = 1; i < layer_commits->len; i++)
    {
      const char *commitid = layer_commits->pdata[i];
      if (!ostree_repo_checkout_tree_at (self->repo, &ocheckout_options,
                                         target_dfd, ".",
                                         commitid,
                                         cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static GOptionContext *
option_context_new_with_commands (Subcommand *commands)
{
  GOptionContext *context;
  GString *summary;

  context = g_option_context_new ("COMMAND");

  summary = g_string_new ("Builtin Commands:");

  while (commands->name != NULL)
    {
      g_string_append_printf (summary, "\n  %s", commands->name);
      commands++;
    }

  g_option_context_set_summary (context, summary->str);

  g_string_free (summary, TRUE);

  return context;
}

static gboolean
submain (struct DlayerOstree *self,
         int    argc,
         char **argv,
         GCancellable *cancellable,
         GError **error)
{
  gboolean ret = FALSE;
  const char *command_name = NULL;
  Subcommand *command;
  int in, out;
  char *prgname;

  /*
   * Parse the global options. We rearrange the options as
   * necessary, in order to pass relevant options through
   * to the commands, but also have them take effect globally.
   */

  for (in = 1, out = 1; in < argc; in++, out++)
    {
      /* The non-option is the command, take it out of the arguments */
      if (argv[in][0] != '-')
        {
          if (command_name == NULL)
            {
              command_name = argv[in];
              out--;
              continue;
            }
        }

      else if (g_str_equal (argv[in], "--"))
        {
          break;
        }

      argv[out] = argv[in];
    }

  argc = out;

  command = commands;
  while (command->name)
    {
      if (g_strcmp0 (command_name, command->name) == 0)
        break;
      command++;
    }

  if (!command->fn)
    {
      GOptionContext *context;
      char *help;

      context = option_context_new_with_commands (commands);

      /* This will not return for some options (e.g. --version). */
      if (option_context_parse (context, NULL, &argc, &argv, cancellable, error))
        {
          if (command_name == NULL)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "No command specified");
            }
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown command '%s'", command_name);
            }
        }

      help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);

      g_option_context_free (context);

      goto out;
    }

  prgname = g_strdup_printf ("%s %s", g_get_prgname (), command_name);
  g_set_prgname (prgname);
  
  if (!command->fn (self, argc, argv, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

int
main (int    argc,
      char **argv)
{
  GError *local_error = NULL;
  GError **error = &local_error;
  GCancellable *cancellable = NULL;
  struct DlayerOstree self = { NULL, };
  
  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  if (!submain (&self, argc, argv, cancellable, error))
    goto out;

 out:
  g_clear_object (&self.repo);
  if (local_error)
    {
      int is_tty = isatty (1);
      const char *prefix = "";
      const char *suffix = "";
      if (is_tty)
        {
          prefix = "\x1b[31m\x1b[1m"; /* red, bold */
          suffix = "\x1b[22m\x1b[0m"; /* bold off, color reset */
        }
      g_printerr ("%serror: %s%s\n", prefix, suffix, local_error->message);
      g_error_free (local_error);
      return 1;
    }
  return 0;
}
