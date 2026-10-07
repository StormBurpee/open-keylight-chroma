"""Actual offline preparer against original synthetic package/image fixtures."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from test_cli import manifest_fixture
import prepare_migration as p
import stock_migration as m


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name) / 'artifacts with spaces é'
        self.folder.mkdir()
        self.source, self.original, _ = manifest_fixture(self.folder)
        self.destination = Path(self.temp.name) / 'plans with spaces'
        self.destination.mkdir()
        self.args = argparse.Namespace(output=self.destination / 'reviewed plan.json',
            target_ip='192.168.86.249', target_name='Fill "studio" é', device_id='keylight-123456',
            source_commit='a' * 40, identity=self.folder/'identity.oklnxp', off1=self.folder/'OFF1.oklnxp',
            low1=self.folder/'LOW1.oklnxp', lighting=self.folder/'lighting.oklnxp',
            esp=self.folder/'open-keylight.bin', assets=self.folder/'asset-manifest.json',
            restore=self.folder/'owner-restore.bin', restore_version='1.3.0.0',
            restore_provenance='Synthetic reviewed full-bank test fixture, no physical restore claimed.')
        self.network = patch('socket.socket', side_effect=AssertionError('No network permitted'))
        self.network.start(); self.addCleanup(self.network.stop)

    def clean_failure(self, exception=ValueError):
        with self.assertRaises(exception): p.prepare(self.args)
        self.assertFalse(self.args.output.exists())
        self.assertEqual(list(self.destination.iterdir()), [])

    def test_exact_paths_hashes_and_independent_loader_validation(self):
        result = p.prepare(self.args)
        self.assertEqual(result['device_operations'], 0)
        self.assertFalse(result['hardware_qualified_by_preparation'])
        self.assertTrue(result['experimental'])
        document = json.loads(self.args.output.read_text(encoding='utf-8'))
        self.assertEqual(document['target']['name'], self.args.target_name)
        self.assertEqual(document['restore']['provenance'], self.args.restore_provenance)
        for group in ('identity', 'OFF1', 'LOW1', 'lighting'):
            self.assertEqual(document['packages'][group]['sha256'], self.original['packages'][group]['sha256'])
        with patch('os.getcwd', return_value=str(self.temp.name)):
            plan = m.load_plan(self.args.output)
        self.assertEqual(result['manifest_sha256'], plan['manifest_sha256'])
        self.assertEqual(plan['target'].name, self.args.target_name)
        self.assertEqual(list(self.destination.iterdir()), [self.args.output])

    def test_wrong_artifact_rejected_without_output(self):
        for field in ('identity', 'off1', 'low1', 'lighting', 'esp', 'assets', 'restore'):
            original = getattr(self.args, field)
            bad = self.folder / 'invalid.bin'; bad.write_bytes(b'not firmware')
            setattr(self.args, field, bad)
            with self.subTest(field=field): self.clean_failure()
            setattr(self.args, field, original)

    def test_role_profile_and_embedded_assets_cannot_be_mixed(self):
        original = self.args.identity
        self.args.identity = self.args.lighting; self.clean_failure()
        self.args.identity = original
        assets = json.loads(self.args.assets.read_text())
        assets['files'][0]['sha256'] = 'b' * 64
        self.args.assets.write_text(json.dumps(assets)); self.clean_failure()

    def test_invalid_target_version_commit_and_provenance_rejected(self):
        for field, value in (('target_ip', '8.8.8.8'), ('device_id', 'unknown'), ('source_commit', 'short'),
                             ('restore_version', '1.3'), ('restore_provenance', 'unknown')):
            original = getattr(self.args, field); setattr(self.args, field, value)
            with self.subTest(field=field): self.clean_failure()
            setattr(self.args, field, original)

    def test_existing_output_preserved_without_validation_or_overwrite(self):
        self.args.output.write_text('keep this file')
        with patch.object(p, 'load_plan', side_effect=AssertionError('Must reject existing output first')):
            with self.assertRaises(FileExistsError): p.prepare(self.args)
        self.assertEqual(self.args.output.read_text(), 'keep this file')

    def test_destination_race_never_overwrites_new_file(self):
        link = p.os.link
        def raced(source, destination):
            Path(destination).write_text('concurrent owner')
            return link(source, destination)
        with patch.object(p.os, 'link', side_effect=raced):
            with self.assertRaises(FileExistsError): p.prepare(self.args)
        self.assertEqual(self.args.output.read_text(), 'concurrent owner')
        self.assertEqual(list(self.destination.iterdir()), [self.args.output])

    def test_failed_validation_flush_or_publication_leaves_no_plan(self):
        for target, error in (('load_plan', ValueError('invalid')), ('fsync', OSError('disk full')),
                              ('link', OSError('filesystem has no atomic hard links'))):
            owner = p if target == 'load_plan' else p.os
            with self.subTest(target=target), patch.object(owner, target, side_effect=error):
                self.clean_failure(type(error))

    def test_source_change_during_validation_does_not_publish(self):
        validate = p.load_plan
        def changed(path):
            self.args.restore.write_bytes(bytes(28672))
            return validate(path)
        with patch.object(p, 'load_plan', side_effect=changed): self.clean_failure()

    def test_cli_preserves_space_quote_and_unicode_arguments(self):
        argv = []
        for name, value in vars(self.args).items():
            argv.extend(['--' + name.replace('_', '-'), str(value)])
        with contextlib.redirect_stdout(io.StringIO()) as output:
            p.main(argv)
        result = json.loads(output.getvalue())
        self.assertEqual(result['target_name'], self.args.target_name)
        self.assertTrue(self.args.output.exists())


if __name__ == '__main__': unittest.main()
